# KTPReHLDS - Claude Code Context

**REQUIRED: Before modifying any C++ in this repo, invoke the `cpp-dev` skill** (`.claude/skills/cpp-dev/SKILL.md`). It carries the fork-delta rules, the cross-thread hazard traps, and the build/verify workflow; do not edit source without it loaded.

🔴 **A design that ships as a document is NOT done** — every proposal in a docs-only PR becomes a
tracked board item in the same act (operator ruling 2026-09-14). See `DESIGN_DOCS_ARE_NOT_DONE.md`.

## Launch-time setup: the commit-sweep guard

Once per clone, before the first commit (linked worktrees share the setting):

```bash
git config core.hooksPath .githooks
```

The main checkout is shared by concurrent sessions, so its index is shared too: a bare `git commit` takes
whatever anyone else has staged, and `git commit -a` takes every modified tracked file. `.githooks/pre-commit`
refuses both in the main checkout whenever anything is staged. Commit by path instead:
`git commit -m "..." -- <paths>` builds a private index from just those paths (a new file still needs
`git add <path>` first). Linked worktrees have their own index and skip the check, so isolated work pays nothing.
Finishing a conflicted merge, revert or cherry-pick legitimately commits the index:
`KTP_COMMIT_SWEEP=1 git commit ...`. A hook already in `.git/hooks/pre-commit` still runs; the guard chains to it.
Test: `sh .githooks/test-pre-commit.sh`.

## Build Command
Build from the repo root with the in-repo CMake script (Linux or WSL):
```bash
bash build.sh -j=$(nproc)
```
Output lands in `build/` — `engine_i486.so` and `hlds_linux`.

**KTP maintainers** run the `build_linux.sh` wrapper in the repo root. It is
**tracked** — the staging path moved to `KTP_STAGING_DIR` (defaulting to the
maintainer box's `KTP DoD Server` tree) and the staging block is skipped entirely
when that directory is absent, so a clean clone builds and simply does not stage.
`KTP_NO_STAGE=1` builds without staging on a box that does have it.

⚠️ **Prefer the wrapper over a bare `build.sh` for anything you intend to ship.**
`build.sh` ends in an unconditional `exit 0`, so a failed `make` still exits 0 and
leaves the previous artifacts in place — which then md5-verify perfectly all the way
to the fleet. The wrapper gates on artifacts newer than the run it just made.

`build.sh` also builds the HLTV Proxy target, and neither it nor `build_linux.sh`
stages the result — both look for `engine_i486.so`/`hlds_linux` only. `proxy.so` has
its own script because it has a different target host:

```bash
bash build_proxy.sh                  # -> build/proxy.so, verified
bash build_proxy.sh --verify <file>  # checks alone, on a binary you did not build
```

It builds in an `ubuntu:24.04` container on purpose. `proxy.so` is one file shared by
all 24 HLTV instances on the **data server** (not the 24 game hosts, so
`stage-wave.py` never touches it), and that host is glibc 2.39 — a build on a newer
distro loads on your machine and fails there, taking every proxy down at the next
restart. The script refuses to emit an artifact that would.

### Build provenance -- ⛔ never build this repo from a git WORKTREE through WSL

🔴 **A WSL build of a Windows git worktree LOSES ITS PROVENANCE AND STILL REPORTS SUCCESS.** The
estate's two standing rules land here together: isolate work in a detached worktree on `C:`, and
compile with `wsl bash -c "cd '/mnt/n/...' && bash build_linux.sh"`. Do both and the engine is built blind.

🔑 **Mechanism.** A worktree's `.git` is a FILE holding `gitdir: N:/Nein_/KTP Git Projects/KTPReHLDS/.git/worktrees/<name>`
-- a **Windows** path. There is no `N:` inside WSL, so git reads the pointer as RELATIVE and fails with
`fatal: not a git repository: /mnt/c/ktp-wt-<name>/N:/Nein_/KTP Git Projects/KTPReHLDS/.git/worktrees/<name>`.
➡️ **The DOUBLED path in that error is the whole diagnosis, and the only place the cause is visible.**

⛔ **The banner and the exit code BOTH lie.** Every git call in `rehlds/version/appversion.sh` has a
silent fallback, so before the gate below it wrote `APP_VERSION "3.22.0.0-dev"` with `APP_COMMIT_SHA ""`,
`make` returned 0, and `build_linux.sh` printed `BUILD COMPLETE!` and `[KTP-BUILD] OK` over it. The binary
is functionally correct and provenance-blind. ⚠️ **`identity is the md5, never the banner` is exactly why
this slipped -- the md5 was fine, so every identity check passed.** ➡️ **A ZERO BUILD NUMBER IS THE TELL**;
it was caught only by reading `3.22.0.0-dev` against the fielded `3.22.0.1076-dev`.

✅ **Controls that separate this from a broken toolchain:** WSL git works perfectly in a REAL checkout on
the same mount -- `git -C "/mnt/n/Nein_/KTP Git Projects/KTPReHLDS/" rev-list --count HEAD` answers -- and the
build number is just `git rev-list --count`, so the correct value was reachable the whole time.
⛔ **`GIT_DIR` is NOT the cause:** `appversion.sh` calls `git -C "$GIT_DIR/"`, which is a chdir and not the
env var, so exporting `GIT_DIR` reproduces a DIFFERENT failure wearing the same message.
⚠️ **Test any WSL git question with a script FILE, never escaped quotes inside `bash -c`** -- a nested-quoting
mistake made a working checkout look broken while this was being diagnosed.

✅ **The fix: clone into the WSL filesystem and build there.** A clone has a real `.git` DIRECTORY, and
native ext4 is also far faster than `/mnt/c` for this tree.

```bash
git clone "/mnt/n/Nein_/KTP Git Projects/KTPReHLDS" ~/ktp-rehlds-build
cd ~/ktp-rehlds-build
# origin/main is a remote-TRACKING ref in the source, not a local branch, so the
# clone does not bring it as a branch and that SHA is otherwise unreachable.
git fetch "/mnt/n/Nein_/KTP Git Projects/KTPReHLDS" refs/remotes/origin/main:refs/heads/buildmain
git checkout buildmain
# APP_COMMIT_URL is built from the remote url, so leaving origin pointing at the
# local path stamps that path into the engine instead of a commit link.
git remote set-url origin https://github.com/afraznein/KTP-ReHLDS.git
KTP_NO_STAGE=1 bash build_linux.sh
```

⚠️ **`MSYS_NO_PATHCONV=1` is required to pass a `/mnt/...` script path through Git Bash to `wsl`** --
without it the argument is rewritten to `C:/Program Files/Git/mnt/...` and the script is simply not found,
which reads as a missing file rather than as a mangled argument.

⛔ **The version step now REFUSES instead of stamping zeros.** `check_git_provenance()` in
`rehlds/version/appversion.sh` fails the `appversion` CMake target -- `make` exits 2 and no `appversion.h` is
written -- whenever `rev-parse --verify HEAD` comes back empty, the clone is shallow, or the commit count is 0 or 1.
🔴 **A SHALLOW CLONE IS THE QUIET ONE: `rev-list --count` ANSWERS 1, NOT 0, SO THE BUILD NUMBER LOOKS
REAL.** Measured on a `--depth=1` clone of this repo: `3.22.0.1-dev`, where the same tree with its history
intact reports `3.22.0.1097-dev`. ➡️ **The fix is `git fetch --unshallow`, or `fetch-depth: 0` on
`actions/checkout`** -- every job in this repo that reaches the version step already sets it, so the hole was
latent here and live for anyone cloning.
✅ **`REHLDS_ALLOW_NO_GIT=1` is the deliberate escape for a build outside any checkout** (source tarball,
vendored copy): it prints the entire refusal anyway and stamps `APP_VERSION "3.22.0.0-nogit"` with
`APP_COMMIT_SHA "nogit"`, so the artifact says what it is wherever it prints its version.
✅ **`rehlds/version/appversion.bat` (the MSVC path) is gated the same way** -- same three conditions, same
message wording, same escape. `rehlds/version/msvc/version.vcxproj` calls it on every `ReHLDS.sln` build, so
MSBuild fails the `version` project rather than stamping a number nothing can trace.

## Project Structure
- `build.sh` - CMake build script (repo root; this is the build)
- `REVIEWS.md` - what has been read against what range, and what was left unread.
  A range absent from it has no recorded review; that is not the same as unreviewed.
- `rehlds/` - Main source directory
- `build/` - Build output (gitignored)
- `msvc/` - Visual Studio project files

⚠️ **This repo mixes line endings within itself, so normalise nothing on the way past.**
`rehlds/engine/sv_phys.cpp` and `sv_phys.h` are CRLF throughout, while `sv_main.cpp`,
`ktp_pushtelemetry.h`, `CHANGELOG.md` and this file are LF. An LF-normalising write to either
`sv_phys` file rewrites every line, which buries the actual change in a whole-file diff and makes
the review worthless. ➡️ **Derive the ending per file before editing** (`git show origin/main:<path> |
tr -dc '\r' | wc -c` against the file's line count), and keep an edit a pure insertion — removing
the added lines must return the original bytes.

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
[KTP_PROFILE] gap=-0.000ms (full - sum of phases)
[KTP_PROFILE] phys_detail_peak: startframe=0.010ms entloop=0.430ms   (.929: interval peaks, tracked inside SV_Physics so paused frames contribute nothing — was an instantaneous last-frame sample)
[KTP_PROFILE] io: logprintf_worst=… conprintf_worst=… logaddr_worst=… file_worst=… fileq_worst=… logq_drops=… ctl_drops=… writer_alive=… condbg_drops=…   (see Async Log-File Writer § for field meanings)
[KTP_PROFILE] send_detail_peak: worst_client=3(name) time=0.290ms clients_sent=11   (.931: interval PEAK; was send_detail, which sampled the boundary frame and never showed a spike)
[KTP_PROFILE] interframe: avg=1.018ms peak=2.400ms
[KTP_PROFILE] net: clients=10 unlag=1 lagcomp_off=1 ignorecmd_hits=2 drops=14 latzero=3 choke_peak=4 loss_worst=6 latency_worst=87.3ms jitter_worst=22.1ms maxunlag=300ms maxunlag_hits=18 maxunlag_excess_worst=412.7ms shadow=0ms shadow_hits=0 shadow_worst=0.0ms updates=9968 subinterval=310 connecting=1 synth_ms=240 interp_cap_hits=0 interp_floor_hits=12 interp_diff_worst=1.7ms lat_pop=1/6/2/1/0/0/0 jit_pop=6/2/1/0/0   (gated on ktp_profile_net; the half of the shot pipeline the CPU records cannot see — see CHANGELOG for field meanings)
[KTP_PROFILE] net_detail: lagcomp_first=3(PlayerA) latency_worst=7(PlayerB) jitter_worst=7(PlayerB) maxunlag_excess_worst=7(PlayerB) shadow_worst=-1(-) drops_worst=7(PlayerB) drops_worst_n=9 latzero_worst=4(PlayerA) latzero_worst_n=2 updates_worst=7(PlayerB) updates_worst_n=998 loss_worst=7(PlayerB) loss_worst_n=6 subinterval_worst=4(PlayerA) subinterval_worst_n=305 synth_worst=7(PlayerB) synth_worst_n=210 interp_diff_worst=4(PlayerA)   (conditional: only when a slot exists to name; latency/jitter names share send_detail_peak's stale-name caveat)
[KTP_PROFILE] rewind: attempts=4812 miss=37 skip=112 depth_worst=118.4ms dist_worst=204.6u
[KTP_PROFILE] net_session: client=7(PlayerB) steamid=<auth id> dur=2412s pkts=241012 cmds=240877 drops=1210 latzero=3 subinterval=0 synth_ms=4180 ignorecmd_hits=2 latency_avg=84.2ms latency_max=191.0ms jitter_avg=3.1ms loss_avg=0.4 loss_max=6   (NOT interval-driven: one line per human client at SV_DropClient_internal, covering the whole connection across maps; needs profiling + ktp_profile_net on at disconnect)
[KTP_PROFILE] push: blocked=1412 rotate=1400 move=12 episodes=6 pushers=3 untracked=0 client_blocks=1405 victims=2   (every interval; blocked pusher frames from SV_PushMove/SV_PushRotate. blocked scales with fps, episodes does not — see CHANGELOG)
[KTP_PROFILE] push_detail: pusher_worst=87(func_door_rotating/door_axis2) pusher_worst_n=1398 pusher_worst_episodes=2 victim_worst=4(PlayerA) victim_worst_n=1398   (conditional: only when something was blocked)
```

⚠️ **`gap=` is zero by construction, in the summary line and in the `[KTP_SPIKE]` line below alike** — each phase is timed as `now - frame_start` with `frame_start` reassigned on the next line, so the six spans telescope to exactly `full` and only float rounding survives the subtraction (which is why the sign varies). It would measure something only if a phase stopped being contiguous with its neighbours.

⚠️ **New fields are only ever appended to the END of `net:` / `net_detail:`** — the aggregator's parsers are not end-anchored and read old and new engines alike only because nothing before the last field moves. Never reorder or rename one.

⚠️ **`rewind:` `attempts` only counts shooters that already passed the `lw && lc` gate** (`SV_SetupMove` returns before `KTP_RewindAttempt`), so `attempts > 0` says nothing about self-handicap — only `lagcomp_off` does. The `interp_*` fields come from the same pass and share that population.

⚠️ **`latzero`, `latency_worst` and `jitter_worst` change meaning at the telemetry-batch build.** Sub-interval packets moved to `subinterval`, and clients not yet `fully_connected` (counted in `connecting`) left every latency-derived field. `latzero + subinterval` is the old `latzero` minus loading clients — never compare the raw series across the boundary.

**Spike alert output (immediate, rate-limited to 1/sec) — the umbrella line always, plus whichever detail lines cleared the phase-share gate:**
```
[KTP_SPIKE] full=12.340ms read=0.150ms phys=0.500ms misc1=0.010ms send=0.100ms post=0.005ms steam=11.500ms gap=0.000ms
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

**Telemetry:** the full `[KTP_PROFILE] io:` line, in order, is:
`logprintf_worst= conprintf_worst= logaddr_worst= file_worst= fileq_worst= logq_drops= ctl_drops= writer_alive= condbg_drops=`
⚠️ `condbg_drops=` is appended AFTER `writer_alive=` deliberately — a new field in the middle would shift every later position for anything that ever reads this line by index.
- `file_worst=` — with async on, measures only the enqueue (~µs); the actual disk write is on the writer thread.
- `fileq_worst=` — writer-thread worst single write per interval (this is where the SSD stall now lands).
- `logq_drops=` — lifetime count of dropped WRITE lines (queue full / writer had no file / write error).
- `ctl_drops=` — (.928) lifetime count of dropped OPEN/CLOSE control ops. Expect 0 forever; nonzero = a whole map's log file may be missing or misrouted (control op couldn't get a ring slot within the bounded retry).
- `writer_alive=` — (.928) 0 only if work is pending AND the writer processed nothing for a full profile interval (a wedged/dead writer). Expect 1.
- `condbg_drops=` — lifetime count of `qconsole.log` lines a BACKGROUND thread emitted before the game thread had opened the file. Only the game thread may enter the FS layer, so a line arriving in that window is counted instead of written. Expect 0. 🔑 Nonzero is also the only in-engine evidence that a background thread has printed at all — which on the fleet means a Steam `sendto()` failed — so read it as a signal, not just a loss. ⚠️ **And a `qconsole.log` line count is NOT comparable across the `Con_DebugLog` thread-safety cut** (`ee2aaf5` + `21ad3e4`): before it there was no thread gate, so an off-thread print called `FS_Open` itself — the line landed in the file when that happened to succeed and vanished with no counter when it did not, and every print formatted through one shared `static char data[8192]` the game thread could interleave into, so a surviving line can be a splice of two. The file answers a different question on either side of that boundary, and in both directions.
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

⛔ **That limitation is about LATENCY and is still open. Its thread-safety half is closed, and the two are easy to conflate.** `Con_DebugLog` is reachable off the game thread — `Con_Printf_internal`'s `else` branch is where a background-thread print lands, which the Steam thread does on every failed `sendto()` — and it used to get there through a shared `static char[8192]` format buffer, an unsynchronized `static FILE*`/`static char[MAX_PATH]` path cache, and the engine FS layer, which is not thread-safe against main-thread map loads. 🔑 **A thread-identity gate cannot fix that**, because that `else` branch is the ordinary destination for every print made while no rcon redirect is active — and the only one a background thread can take, redirect or not — so gating harder only discards the diagnostic. ✅ **Now: stack format buffer, handle published once with release/acquire and never reopened, and `FS_Open`/`FS_Close` confined to the game thread.** 🔑 **The distinction that makes it this small: OPENING walks the search paths and mutates the filesystem's open-file list, but WRITING does not** — `FS_FPrintf`/`FS_Flush` reach only that handle's own `FILE*` (`FileHandle_t` *is* `FILE*`), so they are exactly as thread-safe as the stdio calls underneath, and the write needs no gate at all. ⛔ **Do NOT "resolve the path and `fopen` it" instead, which is the tempting version and silently moves the file:** `FS_Open("at")` writes to the first search path that allows writing, while `FS_GetLocalPath` returns the first one a `stat` HITS — pack files included — so the two disagree whenever a `qconsole.log` exists earlier in the search order, and a pack hit yields a path inside a `.pak` that `fopen` cannot open at all, losing every line for the session. ⚠️ **The off-thread guarantee holds for a thread JOINED before the filesystem is released** — `Steam_Shutdown()` only runs via `Host_ShutdownServer`, which early-returns while `g_psv.active` is false, so a teardown in that window leaves the thread writing through a dead handle, as it always did.

### Host_FilterTime Fix (v3.22.0.904+)
Fixed artificial FPS cap that limited servers to sys_ticrate - 1.

**Original:** `1.0f / (fps + 1.0f)` - At sys_ticrate 1000, capped at ~999 fps
**Fixed:** `1.0 / fps` - Allows true 1000 fps at sys_ticrate 1000

Also changed `fps` variable from `float` to `double` for precision consistency with `realtime`/`oldrealtime`.

### `sys_ticrate` is a frame-REJECTION gate, not the loop rate — so it is the wrong lever

`Host_FilterTime` returns `FALSE` for any frame that arrives sooner than `1/sys_ticrate`
(`host.cpp`): the frame is **discarded**, not scheduled. The loop rate itself comes from the
launcher's sleep mode — the fleet runs `-pingboost 2 -absgrid`, i.e. a ~1 ms iteration — so raising
the cvar only stops good frames being thrown away. ⚠️ **Raising or lowering it does not change
`host_frametime` for anything that is already passing the gate.** Measured: the 2026-05-13 move from
1000 → 1500 took the fleet from ~975 to ~999 fps, i.e. `host_frametime` 1.026 ms → 1.000 ms, a 2.5%
change — and the canary that had been at 690 fps under a gate rejecting ~31% of its frames went to
1000. ➡️ **To change the frame interval you drop `-absgrid`/`-pingboost 2` or set the gate genuinely
low; say which you are testing.** ⛔ **Dropping 1500 → 1000 is not that test** — it re-creates the
rejection regime and nothing else. ⚠️ And ~1 ms frames are what `sv_maxupdaterate 120`, the 300 ms
rewind and the shadow measurement are built on, so this is a trade, not a free knob.

### Pusher displacement scales with 1/fps while `DIST_EPSILON` does not — the door-stuck mechanism

`SV_Physics_Pusher` sets `movetime = host_frametime` (`sv_phys.cpp`) and hands it to `SV_PushMove` /
`SV_PushRotate`, so **per-frame door displacement is inversely proportional to the frame rate**: a
100 u/s door advances ~1.0 unit per frame at 100 fps and ~0.067 at 1000. `DIST_EPSILON = 0.03125`
units (`world.cpp`) is an **absolute** trace backoff, so the share of each frame's push thrown away
rises with fps — about 2.8% at 100 fps against about 28% at 1000. Three facts make it stick rather
than merely slow: `SV_PushMove`/`SV_PushRotate` rewind and call `pfnBlocked` but never resolve
penetration; `move = velocity * movetime` is recomputed fresh each frame, so motion lost to the
epsilon is never repaid; and `SV_PushEntity` does one trace with no slide/retry, so a grazing contact
truncates the whole move. ⚠️ **Plausible and untested as the cause of a stuck report** — the
`push:` counters are the baseline, and `blocked=0` in the window of a report points at the hull test
rather than this path.

📌 **Where the door fights actually land, so a repro starts in the right place.** The live
S10 rotation is **38 `func_door_rotating` against 1 `func_door`** (entity lumps read off the live
BSPs; `mapcycle.txt` governs and `maps.ini` is comment-only), and they are strikingly uniform --
speed 100 deg/s on all 38, 90° travel on 37 of 38, spawnflags 0 on all 38, about a 0.9 s swing
-- so one repro generalises rather than describing one door.
🔑 **`dod_railroad2_s10a` carries 17 of the 38, four times the next map, and the measured
per-map episode baseline follows the DOOR COUNT rather than playtime.** Over 2026-10-01 to 10-05,
attributing each `push:` record to the map named by the preceding `Started map` line, railroad led
on episodes per hour of live half by roughly 4x over `dod_lennon5_b1`, while `dod_anzio` -- the
most-played map in that window -- trailed both by more than an order of magnitude, and
`dod_saints2_b5e`, `dod_armory_b7`, `dod_anjou_a5` and `dod_solitude2` read zero across several
live matches.
⚠️ **The ordering is the finding; the rates are not durable.** Railroad's lead rests on
two live matches in that window, both on one instance, so re-derive before quoting any rate -- and
compare episodes, never raw `blocked`, for the reason recorded above.
✅ **Episodes cluster on a FEW doors and spread across many victims** -- two rotating doors
carry over half of railroad's worst-pusher episodes, four on lennon -- so this is not one player
parked in a doorway, and a repro wants those specific doors rather than the map at large.

⛔ **Two cvars look decisive here and are not, so nobody re-raises them:** `sv_force_ent_intersection`
forces the sphere-intersection check for `SOLID_SLIDEBOX` (player-vs-player) and doors are
`SOLID_BSP`/`MOVETYPE_PUSH`; `sv_rehlds_hull_centering` only fires on a hull whose mins/maxs sum to
zero, which DoD's standing hull is not. Both default `0`.

🔻 **CORRECTED — the pushers are NO LONGER byte-identical to upstream.** `sv_phys.cpp` now carries
`ktp_pusher_clock_quantize` (default `0`) beside the KTP profiling, and upstream has not functionally
touched these functions since 2017. ⚠️ **Carry-the-remainder can still push a player THROUGH
geometry**, which is worse than the bug being fixed — the clock fix below is not that, and does not
repay motion lost to `DIST_EPSILON`.

### A pusher's clock ticks at a different rate than its push moves — `ktp_pusher_clock_quantize`

`pev->ltime` is a **float** that runs for as long as the entity keeps moving or waiting, and
`SV_PushMove` / `SV_PushRotate` advance it with `ltime += movetime` while deriving the push from the
unrounded `movetime`. Past a few hundred seconds the clock cannot hold `ltime + movetime`, and
🔑 **because every step is the same size the loss is a fixed RATIO, not noise** — the clock advances
`round(movetime / ulp) * ulp` each frame. `AngularMoveDone` in the game DLL then assigns the final
angle directly, pushing nobody, so the overshoot comes back as a jump that moves no player.
⚠️ **The sign flips at a clock of 512 s** because `0.001 / ulp` is 32.768 below the boundary (rounds
up, clock runs fast, door stops short) and 16.384 above it (rounds down, clock runs slow, door
overshoots). Simulating the engine's loop for the live rotation's 90° / 100 deg/s door: a 1 ms frame
ends **+2.16°** past the plan at 513 s and **−0.63°** short at 400 s, a 10 ms frame stays inside
0.09° at both, and the quantized path inside 0.01°. ⚠️ **Simulation and unit tests only — never
watched on a live 1000-fps server.**

⛔ **DO NOT write the fix as the obvious two lines.** `float newl = ltime + movetime;
movetime = newl - ltime;` returns `movetime` **unchanged** at `-O2` and above: the engine is 32-bit on GCC's default
`-mfpmath=387`, so the sum stays in an 80-bit x87 register and the rounding being measured never
happens (measured against `-mfpmath=sse`, which quantizes at every level). 🔑 **The `volatile` store
in `KTP_PusherClockStep` IS the mechanism** — removing it changes the shipped `engine_i486.so` md5.
⚠️ **This trap is general: any float-rounding fix in this engine needs a forced store.**

⛔ **And do not add the floor the function looks like it is missing.** When a step falls below half
the clock's resolution the quantize would yield zero, and `KTP_PusherClockStep` hands back the raw
`movetime` instead — it degrades to upstream rather than preventing both branches, deliberately.
🔑 **One return value is both the clock advance and the push distance, so no single number serves
both:** at the threshold one ulp of the clock is 0.00390625 s against a 0.001 s frame, so a floor at
one ulp would run the clock and the push nearly four times fast — a new defect in the exact regime
the fix exists for. ✅ **The branch is unreachable in practice anyway**: it needs one entity's own
clock past 2^15 s at 1000 fps, which that clock only accrues while its door moves or waits, and it
resets at every map load. ⚠️ **The "door freezes with no pending think" reading belongs to the
two-liner above, not to this code** — `if (movetime)` is evaluated *before* the quantize.

### A sliding door repays clock drift with a traced move and a rotating door does not

🔑 **This is a second, independent reason the pusher clock only shows up on rotating doors**, and it
is in the game DLL rather than here, so it is explanation and not a change we can make. The SDK's
`CBaseToggle::LinearMoveDone` measures the residual to its final destination and, above `0.03125` —
the same 1/32 unit as `DIST_EPSILON` — **re-issues the move**, which runs back through the pusher and
therefore traces, pushes and can block. `AngularMoveDone` has no such check: it assigns
`m_vecFinalAngle` and returns. ➡️ **So a linear pusher pays its drift back with a real traced move
while a rotating one pays it with an untraced snap that pushes nobody.** ⚠️ **Read this with the
38-rotating-to-1 locus above rather than instead of it** — the clock bug applies to every
`MOVETYPE_PUSH` entity, and the map content and this asymmetry both point the same way for different
reasons. ⛔ `KTPhlsdk` is what modules are built *against*, not the source of the shipped
`dod_i386.so`, so there is no patch to make on this leg.

### `FCVAR_ARCHIVE` is a complete no-op on this binary — persistence means `dodserver.cfg`

🔴 **A new cvar that needs to survive a `changelevel` cannot get there with a flag.** `FCVAR_ARCHIVE`
is read in exactly one place, `Cvar_WriteVariables`, which is `NOXREF`; both of its callers sit
inside `#ifndef SWDS` and the second also returns early on `g_pcls.state == ca_dedicated` — and
`SWDS` is a compile definition of the engine target in `rehlds/CMakeLists.txt`. ⛔ **So setting it
advertises persistence that does not exist**, which is worse than leaving it off. `FCVAR_SPONLY` is
tested against the client state and is decoration in a dedicated build.
✅ **`FCVAR_SERVER` is the only flag with a live effect here**, and it buys something real for
anything whose output is log lines: `Log_PrintServerVars` dumps the value in every log file's server
cvar block and `Cvar_DirectSet` logs each change, so the arm state of a measurement is in the match
log rather than inferred. ⚠️ **It also broadcasts the change to players; `FCVAR_UNLOGGED` suppresses
the broadcast and the log line in the same branch, so the quiet option is also the blind one.**
➡️ **Persist it in `dodserver.cfg` — an rcon set dies at the next map change**, because a
`changelevel` re-execs that file.

### Is a player left inside a door the engine never saw? — `ktp_pusher_contain_report`

A block is already instrumented, but that path runs only for contacts the hull test **selected and
pushed**, and a detected block reverts both the player and the pusher — so it cannot reach the
reported case, where a door finishes its swing, parks, and a player is inside it with nothing logged.
`ktp_pusher_contain_report 1` (default `0`, `FCVAR_SERVER`, and it also needs `ktp_profile_frame 1`
since the interval record is the only reader) clips every connected client hull against **that pusher
alone**; a `startsolid` is a containment. 🔑 **Read-only in a way that matters:**
`SV_ClipMoveToEntity` returns its trace by value, so unlike `SV_TestEntityPosition` it never calls
`SV_SetGlobalTrace` and cannot disturb the global trace the game DLL reads.

```
[KTP_PROFILE] contain: armed=1 stops=42 tests=168 hits=2 unblocked=2 blind=0 clients=1
```

⚠️ **Three reading rules, and the first is why the record carries fields that look redundant.**
🔑 **`armed=` and `stops=` are what make a zero readable** — `hits=0` otherwise reads identically
whether the probe found nothing, was never armed, or ran through an interval in which no pusher
stopped at all. **`unblocked=` is the subset with no block on record for that pusher this interval**,
which is the population a nudge inside the push path could never reach. ⛔ **`blind=` counts
`unblocked` hits taken after the tracking table had already overflowed, where *no block on record*
may be the table rather than the engine** — so `unblocked` is only trustworthy while `blind=0`.
⚠️ **It samples the STOPPING EDGE only:** a door held open, or a player walking into a door that is
already parked, is never tested, so this bounds the question rather than answering it in general.

### Client updates are quantised to whole frames — `cl_updaterate 102` delivers a phase-locked 100/s

`SV_SendClientMessages` books the next update off the frame it actually went out on:
`next_messagetime = host_frametime + next_messageinterval + realtime` (`sv_main.cpp` ~5573, ~5621).
Because sends land on whole frames, on the 1 ms `-absgrid` grid a requested 102 becomes
`ceil(9.804 ms / 1.000 ms) = 10` frames — **a steady, phase-locked 100 a second, not 102 and not the
~97 an older `Sleep_Select` period produced.** Measured across 24 instances and 13,668 populated
windows: fps 1000.0 at p50.

⚠️ **A fixed-beat scheduler here is a TRADE, not a fix**: advancing `next_messagetime` from its previous
value would hit an average of 102 by alternating 10 ms / 9 ms gaps, giving up the steady cadence.
Measure the delivered rate per client (the `updates=` field on the `net:` record) before and after.

🔑 **Players cannot raise it from their side** — the client caps the `cl_updaterate` it reports at 102,
so "set 120 to guarantee 102" does nothing.

### `-absgrid_probe` is a launcher flag, not an engine flag

Both `-absgrid` and `-absgrid_probe` are parsed by the dedicated launcher (`hlds_linux`, source
`dedicated/src/sys_linux.cpp` and `sys_ded.cpp`), not by `engine_i486.so`. `strings` or grep on the
engine returns nothing and reads like absence; grep `hlds_linux`. They are different tokens, and a
grep for `-absgrid` also matches `-absgrid_probe`, so count the probe token explicitly.

Turning it on is one launch-parameter line in the instance's LinuxGSM `common.cfg` plus the nightly
restart. No engine wave, and it cannot be set by rcon.

With the probe on the loop adds three `clock_gettime` per 1 ms iteration and a histogram written to
`console.log` every 10 s as `[KTP_ABSGRID_PROBE]`; the path without it is untouched. Measured
2026-09-25 on chicago:27018 against its three siblings (about a thousand fps samples each): p05, p50
and p95 were indistinguishable, so no measurable cost.

### A multi-second client stall is the NORM at ~999 fps, and the flood kick is its CONSEQUENCE

*(Negative finding, measured 2026-11-03 and closed. No engine change follows from it. Sample sizes
and controls are in `CHANGELOG.md`'s `### Verified` entry under `[Unreleased]`.)*

Players report a 1-2 s freeze, some of them followed by a disconnect and a chat line about flooding
the server. **Both halves are accounted for and neither is an engine fault.**
✅ **The stall is the ordinary case:** across 1,050 per-client disconnect summaries from non-kicked
clients, the p95 of each client's worst move-packet gap ran 2.9-3.8 s by host while the servers held
~999 fps. A 1-2 s freeze sits inside that distribution, and the server was not behind during it.

🔑 **`SV_ParseMove` credits a client's SILENCE to the rate limiter as commands.**
`numCmdsToIssue = numcmds + net_drop` (`sv_user.cpp`), and `net_drop` is the sequence gap,
`sequence - (chan->incoming_sequence + 1)` (`net_chan.cpp`). The one packet that ends a stall
therefore arrives carrying the whole stall, so the freeze is primary and the kick is downstream of
it. ⛔ **Nothing was queued anywhere** — there is no backlog to find on either side, and looking for
one is the wrong search. `CMD_MAXBACKUP` (64) bounds `numcmds + numbackup` and **not** `net_drop`,
so one packet's contribution to the limiter is unbounded by that guard.

⚠️ **The trip point is not a number of seconds, and reading it as one is the mistake.**
`CheckBurstRate` floors its window at 0.2 s (*"small intervals may give too high rates"*) while
`m_LastCheckTime` advances every 0.5 s, so the credited-command count needed to trip is the ceiling
times somewhere in 0.2-0.5 s, decided only by where in the averaging window the packet lands. At the
fleet's `sv_rehlds_movecmdrate_max_burst 10000` that is over 2,000 at the floor and over 5,000 at a
full window — for a ~91 pps client at one command per packet, roughly 22 s of silence against
roughly 55 s. **At the engine's own default of 5,500 it is over 1,100.** ➡️ **Quote the ceiling and
the window phase, never a duration.**

⛔ **`punish < 0` kicks; `>= 0` AUTO-BANS for that many minutes — and all four punish cvars default
to `5`, not `-1`** (`rehlds_security.cpp`). The kick branch is a **config** choice:
`KTPInfrastructure` `config/online/dodserver.cfg.example` sets all four to `-1`. ⚠️ **A config
regenerated without those lines bans instead of kicking, and since `3.22.0.930` the ban branch is no
longer a silent no-op** — so "we have never seen an auto-ban" is not evidence it is unreachable.
`config/lan` and `config/local` carry only the two `*_burst_punish` lines, leaving both
`*_avg_punish` at `5`.

⛔ **The limiter's own diagnostic is `Con_DPrintf`, gated on `developer != 0`, and `developer` is `0`
fleet-wide.** That line carries the measured rate `(%.1f)` and is the only place the rate exists at
all; what reaches `console.log` is the `SV_DropClient` reason alone. ➡️ **The trip rate is not
recoverable from a fleet log** — do not plan a measurement that needs it.

⛔ **The wording a player quotes is NOT the engine's, so grepping a log for it reads as absence.**
`Stop flooding the server!` is KTPAMXX `antiflood.amxx`'s chat notice (`amx_flood_time`, three
strikes) and **it never kicks anyone**. The engine's strings are
`Kicked for move commands flooding (burst)` and `... (Avg)`. Two subsystems, and only one of them
can produce a disconnect.

⛔ **`drops=` cannot corroborate any of this.** `KTP_NetSampleDrops` discards `net_drop >= 24`
outright, which is exactly the regime that trips the limiter — a seventeen-hundred-packet gap
contributes `0`. ➡️ **Read `gap_max_ms` and `stalls` on the per-client line instead.** ✅ Both are
exact to a 65,535 ms cap, so a multi-second figure there is a measurement and not a saturation.

⛔ **The reported onset window is PERMANENTLY unreadable, and that is settled rather than pending.**
Console retention floors at 2026-10-13 on all 24 instances, so 2026-09-12/13 is gone — and under
`developer 0` it would not have carried the rate even while it existed.

### "Atlanta-only" lag spikes — refuted twice, on two different windows

*(Negative finding, measured 2026-11-03 and closed. No engine change follows from it.)*

One player reported lag spikes ending in a crash to desktop and attributed them to Atlanta.
⛔ **Atlanta is not an outlier, and this has now been refuted on two separate windows — do not raise
it a third time.** Over the retained console window 2026-10-13 to 2026-11-03, timeouts as a share of
all client drops: **Dallas 7.47%** (the highest), Atlanta **5.51%**, Chicago **5.45%**, New York
**4.69%**, Denver **2.99%**. Atlanta against the rest of the fleet is **4.84%**, and on overflow per
session Atlanta is the **lowest** of the three busy regions.

⚠️ **Compare the SHARE of drops, never a raw timeout count** — a raw count tracks how much a region
is played, so the busiest region wins it whatever the netcode is doing. That is the shape the
original claim had.

⚠️ **A real crash to desktop in this period was root-caused separately — a map distributed without
its models — and it does NOT explain this report:** its dates do not cover the reporter's
2026-10-04/10-15. ➡️ Two findings in one window; do not collapse them into one.

### Player movement and step sounds are NOT in this repo — `pm_shared/` is headers only

`rehlds/pm_shared/` ships the GoldSrc movement headers and none of the implementation.
`PM_PlayStepSound`, `PM_UpdateStepSound`, `PM_CheckParamters`, `velwalk` and `velrun` resolve
nowhere in this tree, while `.cpp` files are everywhere else in it — so the grep is not blind, the
code is genuinely absent. DoD owns it inside its own `dod/cl_dlls/client.so` and `dod/dlls/dod.so`,
reached through a function pointer, and **this engine never references a `cl_*` cvar at all**
(`git grep -c cl_upspeed origin/main` against an `sv_maxspeed` control).

⛔ **So a movement, speed or footstep question cannot be answered from this tree, and the wrong
answer it gives looks exactly like a fact.** Readings have been drafted off "our tree says X" and
been wrong twice: the constants in our SDK copy are not the constants DoD ships.

✅ **Read it on the fleet instead, and that is not a workaround — it is better evidence.** Both
binaries are shipped **unstripped with DWARF** (`file` says `with debug_info, not stripped`), their
compilation units include DoD's own `../dod/pm_shared/pm_shared.c`, and `objdump` / `readelf` / `nm`
/ `gdb` are all present on the game hosts. So `gdb -batch -ex 'info line <symbol>'` gives DoD
movement and sound behaviour real `file:line` provenance, and an out-of-range line is refused rather
than answered — which is the control that makes the rest of it trustworthy.

⚠️ **Two instrument traps, both of which returned a clean convincing zero:**
- **`nm` undercounts.** A static function has no symbol entry, so it scores 0 while being present
  and called. Resolve by **relocation name**, not by `nm` and not by absolute address — an
  address grep of a disassembly also returned 0 for a symbol that was certainly there, and only
  its control caught that.
- **`strings` on an `.amxx` is blind** (AMX stores one character per 32-bit cell), so it is no way
  to establish what a plugin enforces. On an ELF `.so` it is fine.

What this engine *does* decide is that it trusts the client's move magnitudes verbatim. `SV_RunCmd`
copies the usercmd straight into `pmove->cmd` (`rehlds/engine/sv_user.cpp`), and the only place the
move axes are zeroed is the paused / `FL_FROZEN` branch. ⚠️ **`sv_maxspeed` and DoD's own parameter
check are UPPER clamps — they scale a command down when it is too fast and leave it alone
otherwise, so nothing anywhere bounds a player who moves slower than normal.** A rationale of the
form "server-side maxspeed covers it" does not cover the slow case; it has been relied on once in
this estate and was wrong at source.

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

## Identifying a deployed engine build

The md5 is the identity, and it is **not reproducible**: the build bakes a GNU build-id and
`__DATE__`, so rebuilding the same commit gives a different hash at the same size. Never identify a
binary by rebuilding a candidate and comparing hashes. Read the commit out of the binary instead:

```bash
grep -a -o -E 'ReHLDS version[^ ]*|ReHLDS/commit/[0-9a-f]*' engine_i486.so
git rev-list --count <commit>    # the build number in the version label comes from this
```

⚠️ Use `grep -a`, not `strings`. `strings` is not installed everywhere, and a missing tool piped into
`grep` gives an empty match that reads as "this binary has no version string". Carry a positive
control either way.

⚠️ The banner is generated from the commit count, so it normally reads *higher* than the label you
cut, but a stale label can also sit below the real build. Re-derive the label from the commit; don't
"correct" it in either direction from the other number.

**`REHLDS_API_VERSION_MINOR` is a fact about a commit, not about the fleet.** Modules gate on it
(dodx checks it in KTPAMXX's `moduleconfig.cpp`). Read it from the commit the *live* binary bakes —
`git show <commit>:rehlds/public/rehlds/rehlds_api.h` — never from this repo's tip.

### Replaced is not loaded

`.new` swaps in with `mv -f`, and a running process keeps the old inode mapped, so *file replaced*
and *file replaced and running* look identical on disk. For each `hlds_linux`, compare the inode it
maps in `/proc/<pid>/maps` with the on-disk inode of that instance's `engine_i486.so`, and look for
`(deleted)` mappings. Before trusting a zero, show the `(deleted)` probe firing on something real on
the same host.

### Smoke-testing under WSL

A loopback run from a drvfs path can segfault in 32-bit `stat` on symlinks — and the live engine
crashes the same way there. Run the live engine in the same harness as a control before concluding
a new build crashes.

### `proxy.so` (HLTV)

`build_linux.sh` collects `proxy.so`. It embeds no version string or SHA, so its md5 is its only
identity. A rebuild differs only in the build-id note and the baked `__DATE__`, so don't restage it
to "refresh" — that churns a pinned hash for no change. `core.so`, `demoplayer.so` and `director.so`
are built too, but nothing collects them.

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

### `sv_unlag_estimator` (default 0) — the steadier estimate, for an A/B only

`1` makes `SV_CalcClientTime` return the median of the last five per-frame samples (first ack of each
frame only), keep the last good value instead of returning 0, and move at most 20 ms per packet;
`sv_unlagsamples` is ignored while it is on. `0` is the stock function, held bit-identical by
`OffPathIsTheOriginalFunction` against a verbatim copy in `ktp_steadyping_tests.cpp` — **edit that copy
in the same change as any edit to the stock body.**

⚠️ **Judge an A/B pair by within-shooter hit share ONLY.** On an estimator instance `latzero`,
`latency_worst`, `jitter_worst` and `subinterval` describe the estimate the rewind is fed, not the
network, and the `rewind:` depth fields are computed from that estimate, so all of them move because
the estimate is smoother.
Comparing any of them across the arms measures the estimator against itself. Procedure and endpoint:
`ESTIMATOR_AB_RUNBOOK.md` in `afraznein/KTPDoDServerConfig`.
