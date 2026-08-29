# KTPReHLDS - Claude Code Context

**REQUIRED: Before modifying any C++ in this repo, invoke the `cpp-dev` skill** (`.claude/skills/cpp-dev/SKILL.md`). It carries the fork-delta rules, the cross-thread hazard traps, and the build/verify workflow; do not edit source without it loaded.

## Build Command
Build from the repo root with the in-repo CMake script (Linux or WSL):
```bash
bash build.sh -j=$(nproc)
```
Output lands in `build/` — `engine_i486.so` and `hlds_linux`.

**KTP maintainers** run a wrapper (`build_linux.sh` in the repo root, gitignored —
it hardcodes a box-specific staging path, so it never ships to a clone). It runs
the same `build.sh` and then copies the artifacts into the local `KTP DoD Server`
test tree. The repo build is `build.sh`.

## Project Structure
- `build.sh` - CMake build script (repo root; this is the build)
- `rehlds/` - Main source directory
- `build/` - Build output (gitignored)
- `msvc/` - Visual Studio project files

## Purpose
Custom ReHLDS (reverse-engineered HLDS) with extension loader. This is the game engine layer that:
- Loads KTPAMXX as a ReHLDS extension (no Metamod required)
- Provides `pfnClientCvarChanged` callback for cvar enforcement
- Enables server pause functionality with HUD updates
- Fixes wall penetration that breaks with Metamod
- Map change interception via `Host_Changelevel_f` hookchain

## KTP Custom Hookchains

### Core KTP Hooks
| Hook | Purpose |
|------|---------|
| `SV_UpdatePausedHUD` | Real-time HUD updates during pause |
| `SV_Rcon` | RCON audit logging (command, IP, validity). **.928:** fires on EVERY attempt with real validity (was success-only); failure audits fire before the packet redirect + throttled ~1/s; password never passed |
| `Host_Changelevel_f` | Console changelevel command interception |
| `hostname_hook` | Cvar hook for hostname changes with optional broadcast |

### Silent Pause Mode (v3.22.0+)
- `ktp_silent_pause` cvar controls client pause notification
- `0` (default): Normal - clients receive `svc_setpause` and show overlay
- `1`: Silent - clients don't receive pause message, no overlay
- Custom HUD via `RH_SV_UpdatePausedHUD` still works in silent mode
- Used by KTPMatchHandler for professional pause experience

### Hostname Broadcast (v3.21.0+)
- `ktp_hostname_broadcast` cvar controls broadcast behavior
- `0` (default): Update serverinfo only (safe, no disruption)
- `1`: Broadcast to clients (causes respawn - NOT for mid-match use)
- Auto-resets to 0 after broadcast (one-shot)

### Frame Profiling (v3.22.0.904+)
Low-overhead profiling to identify performance bottlenecks. Covers the full `SV_Frame_Internal()` loop.

**CVars:**
- `ktp_profile_frame 0/1` - Enable/disable profiling
- `ktp_profile_interval 10` - Seconds between summary logs (default 10)
- `ktp_profile_spike_threshold 5.0` - Log immediate `[KTP_SPIKE]` alert when any single frame exceeds this many ms (0 = disabled)
- `ktp_profile_steam_detail 0/1` - Enable granular Steam_RunFrame() sub-timing (logs `[KTP_PROFILE_STEAM]` when >1ms)
- `ktp_profile_spike_phase_share 0.25` - (.931) Minimum share of the spike frame a phase must own before its `[KTP_SPIKE_<phase>]` detail line is emitted. **`0` = always emit (pre-.931 behaviour, and the rollback lever — no binary swap).**
- `ktp_profile_net 0/1` - Sub-toggle for the `[KTP_PROFILE] net:` record (default **1** — appears wherever profiling is already on). `0` suppresses the lines only; the counters still accumulate at negligible cost.

**Summary output (to server log, every N seconds) — interval lines, plus conditionals:**
```
[KTP_PROFILE] frames=9823 fps=982.3 edicts_max=156
[KTP_PROFILE] avg: read=0.120ms phys=0.450ms misc1=0.005ms send=0.080ms post=0.003ms steam=0.010ms full=0.680ms
[KTP_PROFILE] peak: read=0.450ms phys=1.200ms misc1=0.020ms send=0.300ms post=0.010ms steam=0.050ms full=2.100ms
[KTP_PROFILE] gap=0.012ms (full - sum of phases)
[KTP_PROFILE] phys_detail_peak: startframe=0.010ms entloop=0.430ms   (.929: interval peaks, tracked inside SV_Physics so paused frames contribute nothing — was an instantaneous last-frame sample)
[KTP_PROFILE] io: logprintf_worst=… conprintf_worst=… logaddr_worst=… file_worst=… fileq_worst=… logq_drops=… ctl_drops=… writer_alive=…   (see Async Log-File Writer § for field meanings)
[KTP_PROFILE] send_detail_peak: worst_client=3(name) time=0.290ms clients_sent=11   (.931: interval PEAK; was send_detail, which sampled the boundary frame and never showed a spike)
[KTP_PROFILE] interframe: avg=1.018ms peak=2.400ms
[KTP_PROFILE] net: clients=10 unlag=1 lagcomp_off=1 ignorecmd_hits=2 drops=14 latzero=3 choke_peak=4 loss_worst=6 latency_worst=87.3ms jitter_worst=22.1ms   (gated on ktp_profile_net; the half of the shot pipeline the CPU records cannot see — see CHANGELOG for field meanings)
[KTP_PROFILE] net_detail: lagcomp_first=3(PlayerA) latency_worst=7(PlayerB) jitter_worst=7(PlayerB)   (conditional: only when a slot exists to name; latency/jitter names share send_detail_peak's stale-name caveat)
```

**Spike alert output (immediate, rate-limited to 1/sec) — the umbrella line always, plus whichever detail lines cleared the phase-share gate:**
```
[KTP_SPIKE] full=12.340ms read=0.150ms phys=0.500ms misc1=0.010ms send=0.100ms post=0.005ms steam=11.500ms gap=0.075ms
[KTP_SPIKE_READ] pkts=… recv=… proc=… worst=…
[KTP_SPIKE_PHYS] startframe=… entloop=… paused_startframe=… paused_hud=…
[KTP_SPIKE_IO] logio=… logaddr=… file=… conio=… faults=…   (replaced [KTP_SPIKE_ENT] in .926 — I/O-stall + page-fault attribution)
[KTP_SPIKE_SEND] send=… clients=… worst_slot=… worst=…
[KTP_SPIKE_STEAM] steam=…
```
⚠️ **`[KTP_STEAM_CB_DROPS]` is a LIFETIME counter** — once non-zero it reprints every interval forever. That is intended (the drop already happened and cannot be un-dropped), but it will read as a repeating alarm rather than a new event each time.

⚠️ **As of .931 the detail lines are GATED** on `ktp_profile_spike_phase_share` — before that they
all fired on every spike, which made the aggregator's four per-phase columns four copies of the
spike count. `misc1`/`post`/`gap` have no detail line of their own, so a spike dominated by one of
them clears no phase gate and **`[KTP_SPIKE_IO]` fires as the backstop** — every spike carries at
least one attributing line, and the umbrella line carries every phase unconditionally.
⚠️ `spike_{read,phys,send,steam}` in `ktp_telemetry_metrics` **change meaning at .931** — "spikes"
before, "spikes this phase was material in" after. Do not compare across that boundary.

**Steam detail output (when ktp_profile_steam_detail=1, only when >1ms):**
```
[KTP_PROFILE_STEAM] callbacks=5.200ms sendpackets=0.030ms fragupdate=0.100ms
```

**Metrics tracked:**
- `read` - SV_CheckCmdTimes() + SV_ReadPackets() (network input)
- `phys` - Pause logic + SV_Physics() or pfnStartFrame+UpdatePausedHUD (game simulation)
- `misc1` - SV_RequestMissingResourcesFromClients() + SV_CheckTimeouts()
- `send` - SV_SendClientMessages() (network output)
- `post` - Pause restore + SV_CheckMapDifferences() + SV_GatherStatistics()
- `steam` - Steam_RunFrame() (Steam callbacks, packet send, frag updates)
- `full` - Wall clock time for entire SV_Frame_Internal()
- `gap` - `full` minus sum of all phases (unmeasured overhead / profiling cost)
- `edicts_max` - Peak entity count during interval

**Steam sub-metrics (detail mode):**
- `callbacks` - SteamGameServer_RunCallbacks() (runs every 100ms, prime suspect for lag spikes)
- `sendpackets` - GetNextOutgoingPacket() loop (runs every 10ms)
- `fragupdate` - Player frag updates + server details (runs every 1s)

**Design:** Accumulates per-frame (~nanoseconds overhead per timing point), logs summary every N seconds. Spike alerts fire immediately for individual bad frames. Minimal FPS impact when enabled.

### Async Log-File Writer (v3.22.0.927+)
The synchronous log-file write in `Log_Printf` blocked the game thread up to 167ms on consumer-SSD journal stalls (2026-07-02 telemetry pull + fio). A dedicated writer thread now owns the log `FILE*` via plain stdio; the game thread only enqueues into a 2048-slot ring (full queue = drop + count, never block).

**CVar:** `ktp_log_async 1` (default) — set `0` for the exact legacy synchronous path. Mode latches per log session at `Log_Open` (takes effect next map change).

**Telemetry:** the full `[KTP_PROFILE] io:` line (8 fields, in order) is:
`logprintf_worst= conprintf_worst= logaddr_worst= file_worst= fileq_worst= logq_drops= ctl_drops= writer_alive=`
- `file_worst=` — with async on, measures only the enqueue (~µs); the actual disk write is on the writer thread.
- `fileq_worst=` — writer-thread worst single write per interval (this is where the SSD stall now lands).
- `logq_drops=` — lifetime count of dropped WRITE lines (queue full / writer had no file / write error).
- `ctl_drops=` — (.928) lifetime count of dropped OPEN/CLOSE control ops. Expect 0 forever; nonzero = a whole map's log file may be missing or misrouted (control op couldn't get a ring slot within the bounded retry).
- `writer_alive=` — (.928) 0 only if work is pending AND the writer processed nothing for a full profile interval (a wedged/dead writer). Expect 1.
- `conprintf_worst=` — qconsole.log write cost (see the Con_DebugLog note below). **(.931) Backed by `std::atomic<uint32>` microseconds** — `Con_Printf` is reachable from the Steam and `-netthread` background threads, and the previous plain `double` could tear and print garbage on this very tripwire. ⚠️ Whole microseconds, so a sub-µs call records 0 and this field cannot resolve below 1µs — fine for the millisecond stalls it exists to catch; `logaddr_worst=` — UDP send to a logaddress.

### 3.22.0.928
Engine-side fix wave (see `CHANGELOG.md` for detail):
- **`RH_SV_Rcon` now fires on EVERY rcon attempt** with the real `is_valid` (was success-only, `is_valid` hardcoded true) — bad-password/banned/no-privilege attempts now reach KTPAdminAudit. Failure audits fire BEFORE the packet redirect (no handler output leaks to an unauthenticated prober) and are throttled (global ~1/s; the per-IP tier is best-effort). The password is never included in the audited string.
- **`KTP_ExtensionShutdown`** optional export, dlsym'd + called on each extension in `ReleaseEntityDlls` before the dlclose loop. **.928 alone does NOT close the CHI1-class shutdown segfault** — it installs the hook point; the fix is the paired KTPAMXX export (2.7.21+). Against an older KTPAMXX the callback is inert. `KTP_ClearAllHooks()` (also .928) empties every hookchain registry before the dlclose regardless of extension behavior — the "safe by construction" backstop.
- **`ktp_extension_loaded`** sentinel cvar — counts extensions loaded from `extensions.ini`. **Deploy/restart scripts should assert `ktp_extension_loaded >= 1` via rcon** (the assert is the companion KTPInfrastructure change; the cvar alone detects nothing).
- **Async-writer hardening** — OPEN/CLOSE control ops bounded-retry for a ring slot instead of silently dropping; `ctl_drops=`/`writer_alive=` telemetry added (above).
- **`SetServerPause()` now calls `SV_BroadcastPauseState()`** instead of an inline copy.
- Verify deploys by md5, NOT banner — the `appversion` commit-count skews the banner vs CHANGELOG title.

**Design constraints:** writer thread deliberately bypasses the engine FS layer (not thread-safe vs main-thread map loads); `Log_Open` creates the file via FS then hands the `FS_GetLocalPath`-resolved path to the writer; OPEN/CLOSE are queued ops so rotation ordering holds; `Host_Shutdown` → `KTP_Log_AsyncShutdown()` drains + joins.

**Known-limitation — Con_DebugLog / qconsole.log is NOT covered by the async writer.** With `-condebug` (default on the fleet) + `mp_logecho 1`, every `Log_Printf` also drives `Con_Printf` → `Con_DebugLog("qconsole.log")` → `FS_FPrintf`+`FS_Flush` **synchronously on the game thread** — a second disk sink the async writer never offloaded. Realized cost is small (`conprintf_worst=` ~4.2ms/week worst — `FS_Flush` is `fflush`, page-cache not `fsync`), but it's a bad-writeback-tail exposure on the consumer SSDs. Cheap mitigation if it ever matters: `mp_logecho 0` fleet-wide (the L-log + logaddress still capture everything; only the redundant console echo + its qconsole write go away). Tripwire: watch `conprintf_worst=` on the `io:` line.

### Host_FilterTime Fix (v3.22.0.904+)
Fixed artificial FPS cap that limited servers to sys_ticrate - 1.

**Original:** `1.0f / (fps + 1.0f)` - At sys_ticrate 1000, capped at ~999 fps
**Fixed:** `1.0 / fps` - Allows true 1000 fps at sys_ticrate 1000

Also changed `fps` variable from `float` to `double` for precision consistency with `realtime`/`oldrealtime`.

### Extension Mode Hooks (KTPAMXX/DODX)
| Hook | Purpose |
|------|---------|
| `SV_ClientCommand` | Chat commands, menus (`register_clcmd`, `client_command`) |
| `SV_InactivateClients` | Map change cleanup (`plugin_end`, `client_disconnect`) |
| `SV_ClientUserInfoChanged` | Client info changes (`client_infochanged`) |
| `PF_RegUserMsg_I` | Message ID capture for HUD drawing |
| `PF_changelevel_I` | Game DLL changelevel (`server_changelevel`) |
| `PF_setmodel_I` | Entity model tracking |
| `AlertMessage` | Engine log messages (`register_logevent`) |
| `PF_TraceLine` | TraceLine interception (DODX `TraceLine_Post`) |
| `PF_SetClientKeyValue` | Client key/value changes (DODX) |
| `SV_PlayerRunPreThink` | Player PreThink loop (DODX stats tracking) |

## Build Output
| File | Destination |
|------|-------------|
| `hlds_linux` | Server root |
| `engine_i486.so` | Server root |

## Dependencies
- CMake
- GCC with 32-bit support

## Extension Loading
Extensions are configured in `<gamedir>/addons/extensions.ini` — for DoD,
`dod/addons/extensions.ini`:
```
addons/ktpamx/dlls/ktpamx_i386.so
```
The loader builds the path as `com_gamedir/<line>` (`sys_dll.cpp:1123`), so the
`addons/` prefix is required. A missing or misplaced file returns silently
(`sys_dll.cpp:1077`) and degrades the server to vanilla HLDS.

## Server Deployment

Deploy compiled engine to production servers using Python/Paramiko.

**Server Credentials + host list:** see the root `N:\Nein_\KTP Git Projects\CLAUDE.md` § Server Credentials — do NOT duplicate them here (the old copy showed the pre-2026-05-31 `ktp` password, which was rotated + burned, and omitted Chicago). Active deploy targets are all 5 hosts (Atlanta BM, Dallas, Denver, New York, Chicago), **each running 5 instances on ports 27015-27019**, except Chicago which runs 4 (27015-27018 — the 5th instance was deleted 2026-07-13). Fleet total: 24 instances. The dodserver SSH password rotated 2026-05-31.

**Remote Paths (per instance):**
- `~/dod-{port}/serverfiles/engine_i486.so`
- `~/dod-{port}/serverfiles/hlds_linux` (if updated)
- Deploy as `filename.new` (never overwrite a running `.so`); the nightly restart swaps it.

**IMPORTANT:** After uploading, always set executable permissions:
```bash
chmod +x ~/dod-{port}/serverfiles/engine_i486.so
chmod +x ~/dod-{port}/serverfiles/hlds_linux
```

See `N:\Nein_\KTP Git Projects\CLAUDE.md` for paramiko SSH documentation.

## Related Projects
- `N:\Nein_\KTP Git Projects\KTPAMXX` - AMX Mod X fork (loaded as extension)
- `N:\Nein_\KTP Git Projects\KTPReAPI` - ReAPI fork (engine bridge)
- `N:\Nein_\KTP Git Projects\KTP DoD Server` - Test server with staged binaries

## Key Technical Notes
- Wall penetration breaks when using ReHLDS + Metamod together
- KTP bypasses this by loading KTPAMXX as a ReHLDS extension directly
- This is the foundation of the "No Metamod Required" architecture

## Lag compensation — `sv_unlagsamples` (do not raise it)

*(Relocated 2026-08-29 from the operator's global project context — the engine mechanics belong with
the engine. The live-fleet state stays in the global context: the cvar is set nowhere and is effective
by this engine's default of 1.)*

The engine default is `sv_unlagsamples 1` (`rehlds/engine/sv_user.cpp:68`), and that default is
deliberate. The prior fleet value of 20 was based on a wrong premise: `cl->frames[]` advances per
**client packet** (~100/s), NOT per server frame, so 20 samples = ~200ms of latency smoothing. It also
made the `SV_CalcClientTime` jitter-guard window 20 packets wide — one ping spike in 20 packets
silently returned latency 0.0 = **zero lag compensation for that shot**. Prime suspect for the historic
"random hitreg / high-ping players harder to hit vs legacy" reports.

🔻 **The `[KTP_PROFILE] unlag:` shadow estimator is GONE — do not send anyone to read it.** *(measured
2026-08-26: 0 log files contain `unlag` against a control of 616,161 `KTP_PROFILE` lines in one day;
the live and staged engines carry 9 identical format strings, none unlag, and no gating cvar.)* It was
built, did its job and was **deliberately removed** — see `CHANGELOG.md`: the shadow-20 estimator
validated the `20 → 1` change (**`guard_zero=0` fleet-wide vs `shadow20_zero` in the thousands**), and
it was the dominant cost in `SV_CalcClientTime` while profiling was on.

🔴 **DO NOT READ `guard_zero=0` AS "the guard is fine" — it was guaranteed by construction.** *(traced
2026-08-26, `sv_user.cpp:1124`.)* With `sv_unlagsamples` at 1, `backtrack = 1`; `jitterWindow` is then
1, `minping == maxping`, and the guard `fabs(maxping-minping) <= 0.2` is `0 <= 0.2` — **always true, so
it can never return 0**. The zero says nothing about the network.

⚠️ **And `shadow20_zero` in the thousands says real jitter EXISTS** — at 1 we neither smooth it nor
detect it, since `ping` is one packet's RTT with no averaging. **The middle of the 1..20 range was
never explored.** Re-adding the estimator would need an engine cut.

`config/online/dodserver.cfg.example` carries `sv_unlagsamples 1` with the do-not-raise rationale — keep
those in agreement.
