# [ReHLDS](https://github.com/ReHLDS/ReHLDS) Changelog

**ReHLDS** is a result of reverse engineering of original `HLDS` (build `6152`/`6153`) using `DWARF` debug info embedded into linux version of `HLDS`, `engine_i486.so`.

Along with reverse engineering, a lot of defects and (potential) bugs were found and fixed.

---

## [KTP-ReHLDS `3.22.0.923`] - 2026-04-24

**Suppress KTP_OPCODE alert for HLTV `spawn` opcodes (log-noise cleanup)**

### Fixed
- **`[KTP_OPCODE]` alert no longer fires for HLTV `spawn N ...` commands** (`sv_user.cpp:1975-1998`). HLTV clients hit `WriteSpawn`'s slow-path (iterate-all-entities) inside the game DLL, taking p50=3.2ms in `gamedll` vs p50=0.06ms for real clients — a ~50× asymmetry. Root cost is in `dod_i386.so` (not KTP-controllable, same situation as the 158ms `pfnStartFrame` capture spikes). HLTV reconnects every ~20 min at match half transitions, each reconnect emitted a ~4-5ms `[KTP_OPCODE]` alert — historically this was 85% of total alert volume, pure noise that made real-client alert patterns harder to see. Fine-grained `[KTP_SPAWN]` / `[KTP_WRITESPAWN]` phase profiling still emits unchanged — only the noisy top-level alert is suppressed when `cl->proxy != 0` AND the command starts with `"spawn "`.

### Why
The underlying game-DLL cost is unavoidable for HLTV recording correctness — HLTV needs to see all entity state, so the slow path is legitimate. Filter at the alert-threshold level rather than chasing a binary-patch to `dod_i386.so`. Data captured 2026-04-24 fleet-wide: HLTV SPAWN n=7591 p50=4.818ms p95=5.776ms vs real-client SPAWN n=522 p50=1.812ms p95=5.058ms, writespawn-phase gap entirely in `gamedll` phase (p50=3.197ms HLTV vs 0.062ms real).

### Rollout
Same model as 921 + 922: not auto-staged. Committed to main; bundles with future release once 920 has soaked clean on the fleet.

---

## [KTP-ReHLDS `3.22.0.922`] - 2026-04-23

**Micro-wins from the sv_phys.cpp frame-efficiency pass**

### Fixed
- **`SV_Physics` entity-loop: hoist `gGlobalVariables.force_retouch` and `g_psvs.maxclients`** (`sv_phys.cpp:1494-1510`). The `force_retouch` check is a global float load + compare executed per entity; the field is ~always zero during normal frames (only set transiently during level transitions). Cached as a `const bool` before the loop so the branch folds to a single bool test per entity. `g_psvs.maxclients` is fixed for the server run — hoisted as a `const int`. Combined savings ~5-15ns per entity per frame. At 12 players / peak ~80 active entities mid-match, that's ~0.4-1.2µs/frame — immaterial on its own but a zero-risk cleanup that matches the `sv_timeout` hoist pattern from 3.22.0.916 and the `host_limitlocal` / `sv_failuretime` hoists from 3.22.0.919.

### What was NOT bundled
The same scan surfaced a bigger potential win: hoisting `sv_gravity.value` / `sv_friction.value` / `sv_stopspeed.value` / `sv_bounce.value` out of `SV_AddGravity`, `SV_Physics_Step`, and `SV_FlyMove` via cross-function threading or a frame-scoped global. Rough ballpark savings of 20-80ns per active entity per frame, but the change requires coordinating across 3-4 functions in the physics path — risk-to-reward not proportional for ~1-6µs/frame at peak entity count. Left as a future opportunistic TODO.

### Rollout
Same model as 921: not auto-staged. Committed to main, bundled into a future release once 920 has soaked clean on the fleet. No urgency — the savings are real but immaterial to live gameplay.

---

## [KTP-ReHLDS `3.22.0.921`] - 2026-04-23

**HPAK secondary-path hardening — 5 more `Mem_Malloc` + `Q_memset` sites guarded**

### Fixed
Follow-up to 3.22.0.920's hot-path `Mem_ZeroMalloc` / directory-alloc hardening. Audit of `hashpak.cpp` surfaced five additional sites in less-hot upload and admin-console paths that share the same bug class — `Mem_Malloc` followed by `Q_memset(p, 0, size)` that would SEGV inside glibc `memset` on OOM NULL return. All now `REHLDS_FIXES`-guarded with explicit NULL checks.

- **`HPAK_AddLump:259`** — pDiskData MD5-verification buffer. On OOM, logs and returns; caller's upload is dropped (correct failure mode — can't verify the hash).
- **`HPAK_CreatePak:816`** — same pattern as `HPAK_AddLump`. On OOM, closes the output file handle and returns (no directory entries written yet, safe to abort).
- **`HPAK_CreatePak:844`** — directory entry alloc. On OOM, closes fp and returns. Header was already written; the resulting header-only file fails `HPAK_GetDataPointer`'s `nEntries < 1` validation on next read and is treated as empty — acceptable failure mode.
- **`HPAK_Validate_f:1004`** — per-entry alloc inside the directory-iteration loop (admin `hpkval` command). On OOM, prints `skip (failed to alloc N bytes)` and `continue`s to the next entry; partial validation results are more useful than aborting the whole listing.
- **`HPAK_Extract_f:1153`** — per-entry alloc inside the extraction loop (admin `hpkextract` command). Same `continue`-on-failure pattern; partial extraction is more useful than abort.

### Why
These five sites were explicitly called out in 3.22.0.920's CHANGELOG as "not bundled — less-hot paths, alloc-size/memset-size mismatch makes safe conversion non-trivial." This release keeps the exact existing `Mem_Malloc(size+1)` / `Q_memset(p, 0, size)` behavior on the success path (preserving the intentional trailing-byte uninitialized-space pattern) and only adds explicit NULL guards on the failure path. No behavioral change for OOM-free operation.

### Why NOT bundled with 920
Scope hygiene: 920 shipped with four other changes bundled into the 3 AM ET 2026-04-24 auto-swap (KTPAMXX 2.7.13, fleet JIT activation, KTPMatchHandler 0.10.114, restart-script lockfile patch). Adding a sixth change right before activation would have made regression isolation harder if anything went wrong. 921 lands after 920 has soaked.

### Rollout
Not auto-staged. Committed to KTPReHLDS `main`; hold for a future release bundle once 920 has validated clean on the fleet (3-5 day soak). No urgency — `sv_send_logos 0` fleet-wide means the HPAK upload paths aren't exercised today.

---

## [KTP-ReHLDS `3.22.0.920`] - 2026-04-23

**HPAK defensive hardening — Mem_ZeroMalloc NULL-safety + two hot-path directory alloc guards**

### Fixed
- **`Mem_ZeroMalloc` NULL-safety** (`mem.cpp:36-44`) — stock upstream `Mem_ZeroMalloc` unconditionally calls `Q_memset(p, 0, size)` on the `malloc` result, which dereferences NULL on OOM. Changed to only memset when the alloc succeeded; returns NULL cleanly otherwise. Universal fix — benefits every caller across the engine, not just HPAK. At least one of the fleet-wide segfaults on the HPAK customization path (2026-04 NY/ATL crash clusters) plausibly lands here, since `HPAK_GetDataPointer` allocates up to ~2.3MB for the directory table (`MAX_FILE_ENTRIES=32768 × sizeof(hash_pack_entry_t)`) and under heap fragmentation the call can return NULL.
- **`HPAK_GetDataPointer` directory alloc NULL guard** (`hashpak.cpp:104-117`) — added `REHLDS_FIXES`-guarded NULL check after `Mem_ZeroMalloc`. If the alloc fails, log the entry count, close the file handle, and return FALSE before the subsequent `FS_Read(NULL, ...)` would crash. Belt-and-suspenders alongside the `Mem_ZeroMalloc` fix: now even if a future regression reintroduces the NULL-memset, callers bail cleanly.
- **`HPAK_ResourceForHash` directory alloc NULL guard** (`hashpak.cpp:666-677`) — same pattern as `HPAK_GetDataPointer`. This function uses `Mem_Malloc` (which already returns NULL on failure, unlike the old `Mem_ZeroMalloc`), but the upstream code fed the result directly into `FS_Read` without a NULL check. Both HPAK entry points in the client customization-download path now fail gracefully on OOM.

### Why
Following the 2026-04-22 `sv_send_logos "0"` hotfix, the HPAK segfault remained a latent bug with no code-level fix. A code audit of the `HPAK_ResourceForHash` / `HPAK_GetDataPointer` path (the specific sv_main.cpp:8374-8376 call chain hit by client `dlfile !MD5...` requests) surfaced the `Mem_ZeroMalloc` NULL-memset as the most plausible crash vector, with two additional unchecked-alloc sites feeding `FS_Read(NULL, ...)` as secondary failure modes. All three are now hardened.

This does not necessarily mean OOM was the actual root cause in production (the crashes were intermittent, not heap-pressure correlated). But three real SEGV-on-OOM paths were reachable from the customization hot path, all defensive, all one-line fixes, all guarded under `REHLDS_FIXES`. If the real crash site is elsewhere, the now-enabled fleet-wide core dumps will capture it on the next occurrence under `sv_send_logos 1`, and these defensive fixes can't hurt.

The `sv_send_logos "0"` hotfix stays in place fleet-wide until 920 rolls out and soaks. Toggling `sv_send_logos 1` on a single canary after deploy is the recommended way to re-exercise the HPAK path and confirm no residual crashes (or, if crashes do recur, capture a core dump at the true fault site).

### What was NOT bundled
Five more `Mem_Malloc + Q_memset(p, 0, size)` patterns in `hashpak.cpp` (lines 246, 792, 820, 980, 1129) are functionally `Mem_ZeroMalloc` with the same latent SEGV-on-OOM bug. Not fixed here because the alloc-size / memset-size mismatch (`size+1` alloc vs `size` memset in several of them) makes a safe conversion non-trivial, and the sites are in less-hot paths (upload / admin console commands) where the crash surface is much smaller. Worth a dedicated follow-up PR after 920 soaks.

---

## [KTP-ReHLDS `3.22.0.919`] - 2026-04-23

**Main-loop frame-boundary rewrite + per-frame efficiency + HPAK defensive fix + Linux receive-thread port**

### Added
- **Linux `NET_ThreadMain` port** (`net_ws.cpp`) — the Windows `-netthread` receive-pre-queue thread now has a Linux equivalent using `pthread_create` + `std::mutex`. `NET_ThreadLock`/`Unlock` honor the Linux mutex when the thread is active; `NET_StartThread` / `NET_StopThread` handle thread lifecycle portably. `NET_StopThread` uses a clean shutdown-flag + `pthread_join` (no `TerminateThread` equivalent needed on Linux). Gated behind the existing `-netthread` cmdline flag — off by default. **Mutually exclusive with `-pingboost 3`**: both drive `select()` on the same UDP fds, so `NET_StartThread` detects `-pingboost 3` at init time, logs a warning, and refuses to start the thread. No current burst-traffic pain signal on the fleet, but architecturally symmetric with Windows now.

### Added (experimental)
- **Stage C: `sys_ded.cpp` absolute-time 1ms grid path** (Linux, opt-in via `-absgrid` cmdline flag with `-pingboost 2`) — replaces the fixed `sys->Sleep(1)` call with `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` against a tracked 1ms grid target. The absolute-time kernel path (`hrtimer_start_range_ns(target, 0, HRTIMER_MODE_ABS)`) programs the hrtimer expiry directly — no `now + delta` addition inside the kernel, so scheduler-entry jitter in principle can't accumulate across iterations. **But on tested kernels (6.8.0-110-lowlatency), this primitive does NOT beat the wakeup-latency floor of idle-CPU exit** — measured 643 fps with 1.53ms interframe + recurring 5ms peaks even on a SCHED_FIFO + isolcpus + nohz_full + PR_SET_TIMERSLACK=1 pinned core. The CPU spends most time idle (2.8% utilization) but the hrtimer-fire → thread-runs latency appears to average ~500µs. Likely needs `idle=poll` kernel cmdline or a custom kernel with PREEMPT_RT to reach the theoretical 999 fps. Code is retained for research — gate behind `-absgrid` to opt in; plain `-pingboost 2` keeps the proven `Sleep_Select` path (977 fps baseline, no regression). `prctl(PR_SET_TIMERSLACK, 1)` is set when `-absgrid` is active. New extern `bool g_use_abs_grid` controls the branch in `sys_ded.cpp`.

### Fixed
- **Per-frame `host_limitlocal.value` / `sv_failuretime.value` cvar reads inside the `SV_SendClientMessages` client loop** (`sv_main.cpp:5445, 5476`) — hoisted above the `for (int i = 0; i < g_psvs.maxclients; i++)` loop. Same pattern as the v3.22.0.916 `sv_timeout` hoist. These cvars only change via console command, never mid-frame; per-client dereferences were pure waste. Saves ~2 × 12 clients × 5ns ≈ 120ns per frame at full load.
- **Four sub-function `ktp_profile_frame.value` cvar reads** (`sv_main.cpp:1426, 1746, 4037, 5424`) — replaced with reads of the existing `g_ktp_profiling_enabled` global that `SV_Frame_Internal` sets once per frame (line ~8603). The global existed specifically to eliminate these per-sub-function cvar dereferences but wasn't being consumed. Forward-declared near the top of `sv_main.cpp` so the earlier call sites see the symbol.
- **Four `Cvar_VariableString("hostname")` hash-table lookups** (`sv_main.cpp:1178, 3075, 3150, 6547`) — replaced with direct `host_name.string` reads. `host_name` is a global `cvar_t` declared in `host.h`; the string-keyed lookup was a legacy pattern. Event-driven code paths (not per-frame hot), so the win is cleanup hygiene more than raw cycles — but it also removes a subtle class of bugs where a typo in the cvar name string returns `""` silently.
- **`HPAK_GetDataPointer` defensive fix** (`hashpak.cpp:117-135`) — stock code calls `FS_Read(pbuf, entry->nFileLength, 1, fp)` and `*pbuffer = pbuf` unconditionally after `Mem_Malloc` — if the allocation failed (huge `nFileLength`, low memory, corrupt `custom.hpk`), `FS_Read(NULL, …)` SEGVs the server. Added `break` in the alloc-failure branch to short-circuit before the invalid-pointer write. Dormant bug surfaced by the HPAK crash investigation following the 2026-04-22 `sv_send_logos "0"` hotfix; worth patching while we're in this area of code.

### Why
The 3.22.0.918 release established that `-pingboost 4` (never-sleep) could reach ~999 fps, but at 100% CPU per instance — a poor trade for most fleet deployments. Stage C is the alternative path: keep the sleep, but sleep *until the right moment* rather than *for a fixed duration*. `clock_nanosleep(TIMER_ABSTIME)` was the piece the earlier abs-time `Sleep_BusyWait` experiments were missing — the relative `nanosleep()` primitive had a HZ-tick edge case on the lowlatency kernel that caused multi-ms overshoots on short sleeps. The absolute-time primitive bypasses that entirely by handing the kernel a literal expiry ktime, not a duration to add. Combined with the `Host_FilterTime` 1ns tolerance already in 918 and the zero timerslack, `-pingboost 2` instances now match the `-pingboost 4` fps ceiling.

Supporting wins (cvar hoists, `g_ktp_profiling_enabled` use, direct `host_name.string`) are surgical per-frame cleanups in the same release. The HPAK defensive fix is an opportunistic dormant-bug patch.

---

## [KTP-ReHLDS `3.22.0.918`] - 2026-04-22

**Main-thread offload + new never-sleep pingboost mode**

### Added
- **`-pingboost 4` (never-sleep mode)** — `Sys_Sleep` becomes a no-op; the main loop spins and `Host_FilterTime` rate-gates frame execution. Reaches true ~999 fps at `sys_ticrate 1000` (vs the ~977 ceiling of pingboost 2) at the cost of 100% CPU per instance. Opt-in only via cmdline. Requires exclusive CPU availability (isolcpus + SCHED_FIFO or dedicated VPS); do NOT enable on shared-CPU hosts. Existing `-pingboost 2` stays as the recommended default — it's 977 fps at ~1-3% CPU, which is the better trade for most deployments.

### Changed
- **Steam 5s-timer "refresh server details" moved to background thread** (`sv_steam3.cpp` — `SteamRefresh_Enqueue` / `SteamRefresh_ProcessIfPending`). The 5s block in `CSteam3Server::RunFrame` that called `SetMaxPlayerCount` / `SetBotPlayerCount` / `SetServerName` / `SetMapName` / `SetPasswordProtected` / `SetGameDescription` / `SetGameTags` / per-client `BUpdateUserData` inline on the game frame caused the residual post-3.22.0.913 `steam=3-6ms` spikes that were ~26% of fleet `[KTP_SPIKE]` volume. Main thread now snapshots the needed state (~20µs of cheap reads) and flips an atomic flag; the existing Steam background thread picks up the snapshot within its 50ms poll cycle and publishes to the Steam API off the main thread. Single-slot, lock-free acquire/release handoff; drops the new snapshot if previous is still in flight (next 5s interval re-captures fresh data). NOTE: uses direct `SteamGameServer()->BUpdateUserData` even on `REHLDS_FIXES` builds to bypass the `m_Steam_GSBUpdateUserData` hookchain — plugin handlers on that hook touch main-thread-only state (AMX VM, g_psvs) and are not safe from the background thread. The two low-frequency call sites (`ProcessLogonSuccess` on connect, `NotifyOfLevelChange` on map rotation) still call `SendUpdatedServerDetails()` synchronously on the main thread so plugin hooks continue to fire at those boundaries.
- **`Con_DebugLog` persistent fd** (`sys_dll.cpp:1510`). Stock code opened/wrote/closed `qconsole.log` per log line (3 syscalls). Under `-condebug` + `mp_logecho=1` (default on KTP servers for HLStatsX tailing) that's hundreds of open/close pairs per second. Now caches the `FILE*` across calls, flushes after each write to preserve tail-reader semantics. Cache invalidates and reopens if the `file` argument changes. Also fixes a latent Windows crash bug where the original code called `_write(fd=-1, …)` if the `_open` failed.
- **ProcessConsoleInput rate-limit** (`sys_ded.cpp:155-178`). Stock main loop called `ProcessConsoleInput()` every iteration (→ `kbhit()` → `select()` on stdin). At 100k+ iter/sec in `-pingboost 4` this was 57% kernel time from the syscall alone. Now rate-limited to 50ms cadence (20 polls/sec) via `CLOCK_MONOTONIC`. Admin console input latency is unchanged in practice (imperceptible for human typing); for traditional pingboost modes (0-3) there's no observable impact since they already slept 1ms+ between iterations. Post-fix: pingboost 4 CPU split went from 43% usr / 57% sys → 99.67% usr / 0.33% sys (~172× reduction in syscall load).

### Fixed
- **`Host_FilterTime` float-precision boundary** (`host.cpp:699`). Check is now `(1.0/fps) - 1e-9 > realtime - oldrealtime` instead of `1.0/fps > realtime - oldrealtime`. When a precise-sleep mode lands delta at exactly `1.0/fps`, IEEE 754 double comparison was non-deterministic due to accumulated float precision error in `realtime`, rejecting ~2% of frames and capping measured FPS at ~979 instead of 999 in abs-time sleep experiments. 1ns tolerance (`1e-9s`) beats the boundary reliably while remaining 1000× smaller than the double-precision ulp at 0.001, so no existing pingboost mode's fps gate behavior changes.

### Why (context)
Pre-3.22.0.918 fleet baseline at `sys_ticrate 1000` was a median of 977 fps (see resolved `Why is fleet FPS capped at ~977` TODO). Root cause: the dedicated main loop's fixed `Sleep(1)` + ~25µs of non-sleep work per iteration = ~1.025ms cycle → 977 fps. This release makes several independent improvements:
- Gives operators an opt-in escape to true ~999 fps (pingboost 4) at the cost of 100% CPU
- Removes the residual Steam-timer spikes (26% of fleet spike volume) without any fps tradeoff
- Eliminates hundreds of syscalls/sec from Con_DebugLog under normal operation
- Patches a float-precision fps-gate bug that was blocking future frame-boundary work

The Stage C main-loop frame-boundary rewrite (filed as TODO) is the path to getting to ~999 fps at baseline CPU cost. This release ships the pieces needed to support that work without shipping the rewrite itself.

---

## [KTP-ReHLDS `3.22.0.917`] - 2026-04-19

**Spike-frame phys sub-phase instrumentation**

### Added
- **`[KTP_SPIKE_PHYS]` emitted on spike frames** — `[KTP_PROFILE] phys_detail` has always reported `startframe` + `entloop` values at the periodic log interval, but those are the instantaneous globals *at log time* (whichever frame happened to run last), not the spike frame's values. On the 2026-04-17 ATL2 158ms phys spike the periodic log showed `startframe=0.001ms entloop=0.013ms` — clearly not the spike's values since the spike itself was 158ms. Now every `[KTP_SPIKE]` emission is followed by `[KTP_SPIKE_READ]` *and* a new `[KTP_SPIKE_PHYS]` line capturing the sub-phase values for the exact spike frame.
- **Paused-path sub-phases** — `SV_Physics()` is skipped when the server is paused; the paused else-branch in `SV_Frame_Internal` runs `pfnStartFrame()` + `SV_UpdatePausedHUD()` instead. Those were previously unprofiled, so a 158ms spike on a paused frame would have shown stale `startframe`/`entloop` globals from the last unpaused frame. Added `g_ktp_phys_paused_startframe` and `g_ktp_phys_paused_hud` globals, populated in the paused branch, reset each frame, and emitted alongside the SV_Physics sub-phases in `[KTP_SPIKE_PHYS]`. On any given spike at least one sub-phase pair will be zero, immediately distinguishing which path the spike hit.

### Why
The ATL2 `dod_railyard_s9d` 158ms phys-dominant spike (3-cap Town Square at 21:52:25) left the actual sub-phase breakdown unidentifiable — could have been inside `SV_Physics` or on the paused branch. This patch makes spike-frame phys breakdown visible with every spike event so the next occurrence narrows to a specific sub-phase (game-DLL `pfnStartFrame` cascade, entity physics loop, or `SV_UpdatePausedHUD`). Instrumentation only; no behavior change on non-spike frames.

---

## [KTP-ReHLDS `3.22.0.916`] - 2026-04-02

**Hot-path cvar caching**

### Changed
- **Per-frame cvar cache** — `sv_timeout.value` cached once per frame in `SV_Frame_Internal` instead of re-reading the cvar struct each frame in `SV_CheckTimeouts`. Eliminates redundant cvar dereference from the per-client timeout loop.

---

## [KTP-ReHLDS `3.22.0.915`] - 2026-04-02

**REHLDS_OPT_PEDANTIC re-enabled with wallbang-safe overrides**

### Added
- **REHLDS_OPT_PEDANTIC enabled** — Re-enables 16+ upstream ReHLDS optimizations that were previously disabled entirely due to DoD wall penetration breakage. Two wallbang-unsafe optimizations are manually overridden while all safe ones are active.

### Changed
- **Iterative BSP tree traversal** — `SV_RecursiveHullCheck`, `SV_FindTouchedLeafs`, `SV_LinkContents`, `PM_RecursiveHullCheck` converted from recursive to iterative (tail-call unrolling). Eliminates stack frame overhead on deep BSP trees without changing trace results.
- **Model name hash map** — `SV_FindModelIndex` uses O(1) hash table lookup instead of O(MAX_MODELS) linear search. At 13 clients × ~150 entities × 1000Hz = ~1.95M lookups/sec, this eliminates millions of string comparisons per second.
- **Delta JIT acceleration** — `DELTA_CheckDelta`, `DELTA_WriteDelta`, field set/unset operations use pre-compiled JIT paths for faster entity delta encoding.
- **Challenge circular buffer** — Connection challenge management uses O(1) circular index instead of O(1024) linear scan.
- **User command delta caching** — `MSG_ReadUsercmd` uses cached `g_pusercmddelta` pointer instead of `SV_LookupDelta("usercmd_t")` per frame.
- **CD key MD5 + digest caching** — `SV_WriteFullClientUpdate` sends pre-computed hash instead of recomputing MD5 per update.
- **Packet entity pre-allocation** — Frame entity storage allocated once at MAX_PACKET_ENTITIES, avoiding per-frame reallocation.
- **Point trace specialization** — `SV_Move_Point` fast path for zero-extent traces.

### Preserved (wallbang safety)
- **shouldCollide() kept in early position** — PEDANTIC moves this check after geometric tests which breaks DoD wall penetration. Overridden to always check before `BoundsIntersect`/`SV_CheckSphereIntersection` (world.cpp).
- **AddToFullPack pre-filter removed** — PEDANTIC skips entities without `modelindex` or with `EF_NODRAW` before calling game DLL. Overridden to let game DLL decide all entity visibility (sv_main.cpp).

---

## [KTP-ReHLDS `3.22.0.914`] - 2026-04-02

**Engine hot-path optimizations — lag compensation, compiler, network priority**

### Changed
- **Lag compensation once per packet instead of per cmd** — `SV_SetupMove()` and `SV_RestoreMove()` now run once per packet in `SV_ParseMove()` instead of per-usercmd in `SV_RunCmd()`. At 1000Hz with cl_cmdrate ~100, each packet contains ~10 cmds sharing the same `targettime` (computed from client latency + interp, which doesn't change between cmds). This eliminates ~90% of lag compensation overhead — 13 players' positions no longer get moved back and forth 10 times per packet.
- **Nodelta during pause limited to state transitions** — `SV_EmitPacketEntities()` was forcing full entity state (nodelta) on every frame while paused. Now only forces nodelta for 3 frames after a pause state transition, then resumes delta compression. Eliminates redundant full-state flooding at 1000Hz during paused matches.
- **Compiler: `-march=native -mtune=native`** — Replaced `-mtune=generic -msse3` with native architecture targeting. Enables SSE4.2, AVX, BMI, and other instructions available on server hardware.
- **Compiler: `-flto` (link-time optimization)** — Enables cross-translation-unit inlining for hot paths like `SV_RunCmd` → `SV_SetupMove` → entity iteration.
- **Compiler: `-fno-math-errno`** — Prevents setting errno after math library calls (sqrt, pow, etc.). Engine never checks errno; removing the store allows better optimization of math-heavy paths.
- **IPTOS_LOWDELAY always enabled** — `IP_TOS` with `IPTOS_LOWDELAY` now set on all UDP sockets unconditionally (previously required `-tos` command-line flag). Tells routers and switches to prioritize game packets. TOS failure is now non-fatal (was killing the socket on failure).
- **SV_SetupMove entity early-break** — Entity iteration in lag compensation now breaks out of the inner loop when entity number exceeds `maxclients` instead of continuing through 900+ non-player entities. Players are always entities 1 through maxclients in GoldSrc, guaranteed by entity list ordering.

---

## [KTP-ReHLDS `3.22.0.913`] - 2026-03-27

**Background Steam callback thread — eliminates 3-13ms frame spikes**

### Added
- **Background Steam thread** — `SteamGameServer_RunCallbacks()` and `GetNextOutgoingPacket()` / `NET_SendPacket()` now run on a dedicated background thread (50ms interval) instead of blocking the main game thread. Callback results are queued via a lock-free SPSC ring buffer and processed on the main thread during `RunFrame()`. The IPC pipe read (3-13ms) and outgoing packet loop (1-3ms) that previously caused frame spikes now have zero impact on game frame timing. Steam auth, VAC, and heartbeat functionality is unchanged — all game state access (SV_DropClient, client iteration) remains on the main thread via deferred Process* functions. UDP `sendto()` is atomic on Linux, safe for concurrent calls from game and Steam threads.

### Changed
- **Frag update interval increased from 1.0s to 5.0s** — `SendUpdatedServerDetails()` and per-client `BUpdateUserData()` calls reduced from every 1 second to every 5 seconds. Negligible impact on Steam scoreboard accuracy, reduces a secondary source of frame time variance.

---

## [KTP-ReHLDS `3.22.0.912`] - 2026-03-24

**Physics sub-phase profiling, per-client send profiling, profiler overhead optimization**

### Added
- **Physics sub-phase timing** — `SV_Physics` now reports `pfnStartFrame` time (AMXX plugins + game DLL) separately from the entity physics loop. New periodic log line: `[KTP_PROFILE] phys_detail: startframe=Xms entloop=Xms`. Identifies whether match-time physics overhead is from plugin hooks or engine entity simulation.
- **Per-client send timing** — `SV_SendClientMessages` now tracks the worst (slowest) client each frame. New periodic log line: `[KTP_PROFILE] send_detail: worst_client=N(name) time=Xms clients_sent=N`. Identifies whether HLTV's large signon buffer or specific player connections are the send bottleneck.

### Fixed
- **Double `Sys_FloatTime()` in SV_RunCmd boundaries** — Called `clock_gettime` twice at both prethink and pmove/postthink boundaries per cmd per player (thousands/sec with profiling on). Now captures once and reuses at both boundaries.
- **Unconditional global writes gated on profiling** — 7 read-detail globals in `SV_ReadPackets` and 3 send-detail globals in `SV_SendClientMessages` were zeroed every frame even when profiling was disabled. Now only written when profiling is active, eliminating 10,000 cache-dirtying writes/sec on production servers.
- **Cvar dereference consolidated into single global** — `ktp_profile_frame.value` was independently read in `SV_RunCmd`, `SV_ParseMove`, per-opcode dispatch, and `SV_Physics` (10,000+ reads/sec across all sites). Now set once per frame in `SV_Frame_Internal` via `g_ktp_profiling_enabled` global, eliminating all downstream cvar dereferences.
- **Per-client send cvar read hoisted outside loop** — Was reading `ktp_profile_frame.value` per client iteration (13x/frame). Now read once before the loop.
- **Gap/spike alert timestamps reused** — `Sys_FloatTime()` calls in rate limiters replaced with already-captured `ktp_t_full_start` / `ktp_t_frame_end` values.
- **Steam/frame-end profiling blocks merged** — Two back-to-back `if (ktp_profiling)` blocks with separate `Sys_FloatTime()` calls merged into one, eliminating a redundant syscall per frame.
- **Steam detail start timestamps reuse `fCurTime`** — Three `Sys_FloatTime()` calls in `Steam_RunFrame` detail profiling (frag, callback, sendpacket start points) replaced with the already-captured `fCurTime` value.

---

## [KTP-ReHLDS `3.22.0.911`] - 2026-03-24

**Profiling accuracy, pause network efficiency, rate limiter correctness**

### Fixed
- **`SetServerPause` debug log demoted to developer-only** — `Con_Printf` in `SetServerPause()` was synchronous console output on every pause/unpause transition. Changed to `Con_DPrintf` to avoid production log noise.
- **Double `Sys_FloatTime()` calls eliminated in SV_ReadPackets** — Per-packet profiling blocks called `Sys_FloatTime()` twice (once to measure, once to reset). Now reuses the captured timestamp, eliminating ~780 redundant clock reads/sec with 12 players.
- **Pause force-send limited to clients with pending data** — During temporary unpause (for chat), `send_message = TRUE` was set for ALL clients every frame at 1000Hz, bypassing rate limiting. Now only forces send when the client's reliable message buffer has data (`cursize > 0`).
- **`netchan.cleartime` fudge removed** — Replaced `realtime - 0.001` magic offset with exact `realtime` reset. Changed comparison from `>=` to `>` to match `Netchan_CanPacket` semantics cleanly.
- **Rate limiter clock source unified** — Spike and gap alert rate limiters used `realtime` while measurements used `Sys_FloatTime()`. Under load spikes `realtime` can lag, causing over-suppression. Both now use `Sys_FloatTime()` consistently.
- **Profile interval minimum corrected** — Comment said "minimum 1 second" but code fell back to 10.0 when value < 1.0. Now correctly falls back to 1.0.
- **String command rate limiter bypass scoped to current client** — During temporary unpause, rate limiter was bypassed for ALL clients. Now only bypasses for the client whose packet is being processed (`host_client`).
- **Interframe average uses dedicated frame counter** — Was dividing accumulated interframe time by total frames, but only frames with `ktp_t_interframe > 0` contributed. Now tracks a separate `g_ktp_profile_acc_interframe_count` for accurate averaging.
- **Stale read-detail globals cleared unconditionally** — `g_ktp_read_pkt_count` etc. were only written when profiling was enabled. If profiling was toggled mid-session, spike logger could print stale values. Now reset at the top of every `SV_ReadPackets()` call.
- **Engine command frametime check uses `g_ktp_temporary_unpause`** — `SV_ParseStringCommand` case 1 checked `g_psv.paused` to decide whether to save/restore `frametime`, but `g_psv.paused` is always 0 during the temporary unpause window. Now checks `g_ktp_temporary_unpause` to correctly detect paused-server frames.
- **Force-send `else if` braces added** — Misleading indentation on the `else if` branch in `SV_SendClientMessages` could cause future misreads. Added explicit braces for safety.
- **Ordering-critical comment on `SV_IsSimulating` call** — `SV_IsSimulating()` must be called before `g_psv.paused = 0` to capture the real pause state. Added `ORDERING CRITICAL` comment to prevent accidental reordering.

---

## [KTP-ReHLDS `3.22.0.910`] - 2026-03-19
- Raised `sv_unlagsamples` cap from 16 to 64 (full `SV_UPDATE_BACKUP` frame buffer). At 1000Hz tickrate, the old cap of 16 only covered 16ms of ping history — insufficient for meaningful smoothing. Now allows up to 64ms averaging window, tunable via cvar without engine rebuilds.
- Scaled jitter detection window in `SV_CalcClientTime()` to match the averaging window. The hardcoded 4-frame min/max check (4ms at 1000Hz) was too narrow, causing the 200ms jitter safety valve to trigger on normal ping variance. Now uses the same window as `sv_unlagsamples`.

---

## [KTP-ReHLDS `3.22.0.909`] - 2026-03-13
- Added `[KTP_SPAWN]` sub-phase profiling to `SV_Spawn_f_internal` — breaks down spawn time into: signon copy, WriteSpawn, voice codec, fragment creation, fragment send. Includes `hltv=` flag and signon buffer size.
- Added `[KTP_WRITESPAWN]` sub-phase profiling to `SV_WriteSpawn` — breaks down into: game DLL init (`pfnClientPutInServer`), client update loop (`SV_FullClientUpdate`), lightstyles + client data, finalization. Both log lines fire when total > 1ms, controlled by `ktp_profile_frame` cvar.

---

## [KTP-ReHLDS `3.22.0.908`] - 2026-03-11
- Added command string to `[KTP_OPCODE]` log for `clc_stringcmd` opcodes — logs the actual command (e.g., `fullupdate`, `status`) when processing takes >1ms, enabling diagnosis of 80-169ms stalls

---

## [KTP-ReHLDS `3.22.0.904+`] - 2026-02

**Full Frame Profiling, Netcode Limits, CPU-Time Instrumentation**

### Added

#### 6-Phase Frame Profiling System
- **`ktp_profile_frame`** - Enable/disable frame time profiling (0/1)
- **`ktp_profile_interval`** - Seconds between summary logs (default: 10)
- **`ktp_profile_spike_threshold`** - Immediate `[KTP_SPIKE]` alert when any frame exceeds N ms (0 = disabled)
- **`ktp_profile_steam_detail`** - Granular `[KTP_PROFILE_STEAM]` sub-timing when Steam_RunFrame > 1ms

**Summary output (every N seconds):**
```
[KTP_PROFILE] frames=9823 fps=982.3 edicts_max=156
[KTP_PROFILE] avg: read=0.120ms phys=0.450ms misc1=0.005ms send=0.080ms post=0.003ms steam=0.010ms full=0.680ms
[KTP_PROFILE] peak: read=0.450ms phys=1.200ms misc1=0.020ms send=0.300ms post=0.010ms steam=0.050ms full=2.100ms
[KTP_PROFILE] gap=0.012ms (full - sum of phases)
```

**Spike alert output (immediate, rate-limited to 1/sec):**
```
[KTP_SPIKE] full=12.340ms read=0.150ms phys=0.500ms misc1=0.010ms send=0.100ms post=0.005ms steam=11.500ms gap=0.075ms
[KTP_SPIKE_READ] pkts=5(cl=3,conn=1,frag=2) recv=0.010ms proc=3.200ms worst=2.800ms
```

**Steam detail output (when ktp_profile_steam_detail=1, only when >1ms):**
```
[KTP_PROFILE_STEAM] callbacks=5.200ms sendpackets=0.030ms fragupdate=0.100ms
```

**Phases tracked:**
- `read` - SV_CheckCmdTimes() + SV_ReadPackets() (network input)
- `phys` - Pause logic + SV_Physics() (game simulation)
- `misc1` - SV_RequestMissingResourcesFromClients() + SV_CheckTimeouts()
- `send` - SV_SendClientMessages() (network output)
- `post` - Pause restore + SV_CheckMapDifferences() + SV_GatherStatistics()
- `steam` - Steam_RunFrame() (callbacks, packet send, frag updates)
- `full` - Wall clock for entire SV_Frame_Internal()
- `gap` - `full` minus sum of all phases (unmeasured overhead)

#### SV_ReadPackets Detail Profiling
Per-packet timing inside the read loop: recv vs process time, packet type counts (client/connectionless/fragment), worst single packet time. Results stored per-frame, logged on spike alerts.

#### SV_ParseMove / SV_RunCmd Sub-Phase Instrumentation
- `[KTP_PARSEMOVE]` logged when any client's ParseMove > 1ms, with CPU-time breakdown:
  - `setup` - viewangles, cmd decompression, time base
  - `runcmd` - total SV_RunCmd phase, broken into:
    - `prethink` - pfnCmdStart + PreThink + Think
    - `pmove` - player movement setup + pfnPM_Move + movevars sync
    - `postthink` - result copyback + touches + PostThink + CmdEnd
  - `cpu` - CLOCK_THREAD_CPUTIME_ID (distinguishes CPU work from OS scheduling stalls)

#### Per-Opcode Client Message Timing
- `[KTP_OPCODE]` logged when any client opcode handler > 1ms (clc_move, clc_stringcmd, etc.)

#### Sys_ThreadCpuTime()
- New function using `CLOCK_THREAD_CPUTIME_ID` for CPU-only timing (excludes kernel descheduling)
- Falls back to `Sys_FloatTime()` on Windows

### Changed

#### Rate Limits Raised
- **MAX_RATE** raised 100,000 → 1,000,000 (net.h) — allows `rate 1000000` for LAN/high-bandwidth clients
- **HLTV MAX_PROXY_RATE** raised 100,000 → 1,000,000 (Proxy.h)
- **HLTV MAX_PROXY_UPDATERATE** raised 100 → 200 (Proxy.h)
- **HLTV interp buffer** reduced from 50ms to 15ms (Proxy.cpp: `ex_interp` formula changed from `+0.05f` to `+0.015f`)

### Fixed

#### Host_FilterTime FPS Precision
- **Original:** `1.0f / (fps + 1.0f)` — at sys_ticrate 1000, capped at ~999 fps
- **Fixed:** `1.0 / fps` — allows true 1000 fps at sys_ticrate 1000
- Changed `fps` variable from `float` to `double` for precision consistency

### Compatibility Notes

- **No API changes** - backwards compatible with existing plugins
- **Production safe** - profiling overhead negligible when cvars disabled

---

## [KTP-ReHLDS `3.22.0.903-dev+m`] - 2026-01

**Silent Pause Mode** - Hide client pause overlay while maintaining custom HUD functionality.

### Added

#### New Cvar: ktp_silent_pause
- **`ktp_silent_pause`** - Controls whether clients receive `svc_setpause` messages
  - `0` (default): Normal behavior - clients receive pause notification and show overlay
  - `1`: Silent mode - server is paused but clients don't receive `svc_setpause`
  - Prevents the blocky "PAUSED" client overlay from appearing
  - Custom HUD updates via `RH_SV_UpdatePausedHUD` still work normally
  - Physics remain frozen server-side

#### New Function: SV_BroadcastPauseState()
- Centralized function for broadcasting pause state to clients
- Respects `ktp_silent_pause` cvar setting
- Used by all pause-related functions:
  - `SetServerPause()` (ReAPI native)
  - `Host_TogglePause_f()` (pause command)
  - `Host_Pause_f()` (setpause command)
  - `Host_Unpause_f()` (unpause command)

### Technical Details

```cpp
// Silent pause prevents svc_setpause broadcast to clients
extern cvar_t ktp_silent_pause;

void SV_BroadcastPauseState(qboolean paused) {
    if (ktp_silent_pause.value != 0.0f) {
        // Skip broadcast - clients won't see pause overlay
        return;
    }
    // Normal broadcast to all connected clients
}
```

### Use Case

- KTPMatchHandler sets `ktp_silent_pause 1` before pausing
- Server physics freeze, game is paused
- Clients do NOT receive `svc_setpause` message
- No blocky "PAUSED" overlay appears on client screens
- Custom HUD countdown (via `RH_SV_UpdatePausedHUD`) displays instead
- Professional pause experience for competitive matches

### Compatibility Notes

- **Requires KTPMatchHandler 0.10.65+** for automatic silent pause integration
- **Backwards compatible** - default behavior unchanged (`ktp_silent_pause 0`)
- Works with existing `RH_SV_UpdatePausedHUD` hook

---

## [KTP-ReHLDS `3.21.0.902-dev+m`] - 2026-01

**Hostname Broadcast Infrastructure** - Cvar hook for hostname changes with optional client broadcast.

### Added

#### New Cvar Hook: hostname
- **`hostname_hook`** - Cvar hook triggered when `hostname` cvar changes
  - Always updates serverinfo string (visible to new connections)
  - Optional client broadcast via `ktp_hostname_broadcast` cvar
  - Enables plugins to update server name dynamically

#### New Cvar: ktp_hostname_broadcast
- **`ktp_hostname_broadcast`** - Controls hostname change broadcast behavior
  - `0` (default): Only update serverinfo, no client broadcast (safe)
  - `1`: Broadcast `svc_serverinfo` to all connected clients (updates scoreboard)
  - Auto-resets to 0 after broadcast (one-shot mode)

### Technical Details

```cpp
// hostname cvar hook registered in Host_InitLocal()
Cvar_HookVariable(host_name.name, &hostname_hook);

// Callback updates serverinfo and optionally broadcasts
void hostname_hook_callback(cvar_t *cvar) {
    Info_SetValueForKey(Info_Serverinfo(), "hostname", cvar->string, MAX_INFO_STRING);

    if (ktp_hostname_broadcast.value == 1.0f) {
        // Send SV_SendServerinfo to all connected clients
        // Auto-disable after broadcast
    }
}
```

### Known Limitations

- **Broadcast causes client reinitialization** - `SV_SendServerinfo` triggers full client reinit (respawn + MOTD + team/class menus)
- **NOT RECOMMENDED for mid-match use** - Client disruption makes this unsuitable for live matches
- **Use cases**: Practice mode, empty server, between matches
- **Workaround**: Hostname changes without broadcast only visible to new connections and on map change

### Compatibility Notes

- **Safe by default** - `ktp_hostname_broadcast 0` causes no disruption
- **AMXX plugin control** - Set `ktp_hostname_broadcast 1` only when safe (e.g., practice mode)

---

## [KTP-ReHLDS `3.20.0.896-dev+m`] - 2026-01

**Map Change Interception & RCON Audit Hooks** - Console changelevel and RCON command hookchains.

### Added

#### New Hookchain: Host_Changelevel_f
- **`Host_Changelevel_f`** - Console changelevel command interception
  - Called when `changelevel` command is executed (including `server_cmd("changelevel ...")`)
  - Parameters: `map` (target map name), `startspot` (landmark, usually empty)
  - Return `HC_SUPERCEDE` to cancel the map change
  - Used by KTPMatchHandler for:
    - Overtime continuation (stay on same map instead of rotating)
    - Match state persistence across map changes
    - Score carryover between halves

#### New Hookchain: SV_Rcon
- **`SV_Rcon`** - RCON command audit logging
  - Called when an RCON command is received by the server
  - Parameters: `command` (string), `from_ip` (string), `is_valid` (bool)
  - Enables security logging of all RCON attempts with source IP
  - Used by KTPAdminAudit for comprehensive RCON audit trails
  - Captures both successful and failed authentication attempts

### Technical Details

```cpp
// Host_Changelevel_f split into hookchain pattern
void Host_Changelevel_f_internal(const char *map, const char *startspot);

void Host_Changelevel_f(void) {
    // ... validation and argument parsing ...
    g_RehldsHookchains.m_Host_Changelevel_f.callChain(
        Host_Changelevel_f_internal, _level, startspot);
}

// SV_Rcon hookchain for audit logging
void SV_Rcon_internal(const char *command, const char *from_ip, bool is_valid);
```

### Compatibility Notes

- **Requires KTP-ReAPI 5.29.0.362-ktp+** for hook exposure
- **Used by KTPMatchHandler 0.10.30+** for OT map handling
- **Used by KTPAdminAudit 2.2.0+** for RCON audit logging

---

## [KTP-ReHLDS `3.19.0.895-dev+m`] - 2025-12-21

**Admin Command Blocking & Pause System Improvements** - Secure kick/ban enforcement and network stability.

### Added

#### Blocked Console Kick/Ban Commands
All kick and ban console commands now blocked at engine level for audit trail enforcement:

- **`Host_Kick_f`** - `kick` command blocked, directs to `.kick` in-game
- **`SV_BanId_f`** - `banid` command blocked, directs to `.ban` in-game
- **`SV_RemoveId_f`** - `removeid` command blocked
- **`SV_AddIP_f`** - `addip` command blocked, directs to `.ban` in-game
- **`SV_RemoveIP_f`** - `removeip` command blocked

**Purpose:**
- Prevents untraceable RCON/console kicks and bans
- Forces all player removals through KTPAdminAudit plugin
- Ensures full audit trail with admin name, SteamID, and timestamp
- Logs all blocked attempts for security review

**Console Output:**
```
Kick command disabled. Use .kick in-game (requires admin flag).
Blocked kick attempt: kick #123
```

### Fixed

#### Force Nodelta During Pause
- **`SV_EmitPacketEntities`** now forces `sv_packet_nodelta` when `g_ktp_temporary_unpause` is active
- **Issue:** During pause, delta sequences become stale causing `cl_flushentitypacket` warnings on clients
- **Fix:** Use nodelta packet type during temporary unpause frames
- **Result:** Cleaner network handling, no client-side warnings during pause HUD updates

#### Pause State Restoration Logic
- **`SV_Frame_Internal`** improved pause state restoration
- **Issue:** Plugin pause state changes during frame could conflict with restoration logic
- **Fix:** Check `g_ktp_temporary_unpause` flag when deciding to restore; if plugin cleared it, pause change was intentional
- **Result:** Plugins can now reliably change pause state without race conditions

### Technical Details

```cpp
// SV_EmitPacketEntities - Force nodelta during pause
sv_delta_t deltaType;
if (client->delta_sequence == -1 || g_ktp_temporary_unpause) {
    deltaType = sv_packet_nodelta;
} else {
    deltaType = sv_packet_delta;
}

// SV_Frame_Internal - Improved restoration logic
if (shouldRestorePause && g_ktp_temporary_unpause) {
    g_psv.paused = wasPaused;
}
g_ktp_temporary_unpause = 0;  // Always clear at frame end
```

### Compatibility Notes

- **Requires KTPAMXX 2.6.0+** with `ktp_drop_client` native for kick functionality
- **Requires KTPAdminAudit 2.1.0+** for menu-based kick/ban
- **Backwards compatible** with existing plugins

---

## [KTP-ReHLDS `3.18.0.894-dev+m`] - 2025-12-16

**DODX Extension Mode Support** - PlayerPreThink hookchain for stats tracking.

### Added

#### New Hookchain for DODX Extension Mode
- **`SV_PlayerRunPreThink`** - PlayerPreThink engine hook
  - Enables DODX `FN_PlayerPreThink_Post` functionality in extension mode
  - Called every frame for each connected player before player think
  - Used by DODX for stats tracking loop, shot detection, and player initialization
  - Parameters: `edict_t *player, float time`

### Technical Details

#### Hook Chain Addition to `IRehldsHookchains`
```cpp
virtual IRehldsHookRegistry_SV_PlayerRunPreThink* SV_PlayerRunPreThink() = 0;
```

#### Implementation
- `SV_PlayerRunPreThink()` split into internal function and hookchain wrapper
- Original engine logic preserved in `SV_PlayerRunPreThink_internal()`
- Hookchain called in wrapper, allowing modules to intercept before/after

### Compatibility Notes

- **Requires KTPAMXX 2.4.0+** for full DODX extension mode support
- **Backwards compatible** with existing extensions
- **API version unchanged** - existing extensions work without recompilation

---

## [KTP-ReHLDS `3.17.0.893-dev+m`] - 2025-12-08

**Extension Mode Support** - Additional API hooks for KTPAMXX and DODX compatibility.

### Added

#### New Hookchains for Extension Mode
- **`AlertMessage`** - Engine log message hook
  - Enables `register_logevent` functionality in extension mode
  - Intercepts all engine AlertMessage calls with formatted output
  - Parameters: `ALERT_TYPE atype, const char *szOut`

- **`PF_TraceLine`** - Engine TraceLine hook
  - Enables DODX `TraceLine_Post` functionality in extension mode
  - Called for all `PF_traceline_DLL` calls with full TraceResult
  - Parameters: `const float *v1, const float *v2, int fNoMonsters, edict_t *pentToSkip, TraceResult *ptr`

- **`PF_SetClientKeyValue`** - Client key/value hook
  - Enables DODX `SetClientKeyValue` functionality in extension mode
  - Called when client userinfo key/value pairs are modified
  - Parameters: `int clientIndex, char *infobuffer, const char *key, const char *value`

### Technical Details

#### Hook Chain Additions to `IRehldsHookchains`
```cpp
virtual IRehldsHookRegistry_AlertMessage* AlertMessage() = 0;
virtual IRehldsHookRegistry_PF_TraceLine* PF_TraceLine() = 0;
virtual IRehldsHookRegistry_PF_SetClientKeyValue* PF_SetClientKeyValue() = 0;
```

### Compatibility Notes

- **Requires KTPAMXX 2.1.0+** for extension mode features
- **Backwards compatible** with standard ReAPI/AMXX plugins
- **API version unchanged** - existing extensions work without recompilation

---

## [KTP-ReHLDS `3.16.0.892-dev+m`] - 2025-12-06

**Extension Mode Support** - Major API additions for KTPAMXX extension mode compatibility.

### Added

#### New Hookchains for Extension Mode
- **`SV_ClientCommand`** - Client command processing hook
  - Enables `register_clcmd`, menu systems, and `client_command` forward in extension mode
  - Called when clients send commands (chat, menuselect, etc.)
  - Allows KTPAMXX to process chat commands (`/start`, `.start`) and menu selections

- **`SV_InactivateClients`** - Map change deactivation hook
  - Called at the START of map change sequence (before clients are inactivated)
  - Enables KTPAMXX to fire `plugin_end` and `client_disconnect` forwards during map change
  - Critical for proper plugin cleanup during map transitions

- **`SV_ClientUserInfoChanged`** - Client info change hook
  - Called when client userinfo is modified
  - Enables `client_infochanged` forward in extension mode

- **`PF_RegUserMsg_I`** - User message registration hook
  - Allows intercepting message ID registration
  - Used by KTPAMXX to capture message IDs for HUD drawing

- **`PF_changelevel_I`** - Server changelevel hook
  - Called when server changes level
  - Enables `server_changelevel` forward

- **`PF_setmodel_I`** - Entity setmodel hook
  - Called when entity model is set
  - Enables entity model tracking

### Fixed

- **`RegUserMsg_internal` now searches both message lists**
  - **Issue**: Looking up existing message IDs via `REG_USER_MSG` created duplicates with IDs 130+
  - **Problem**: `RegUserMsg_internal` only searched `sv_gpUserMsgs` (messages already sent to clients)
  - **Fix**: Now also searches `sv_gpNewUserMsgs` (newly registered messages not yet sent)
  - **Result**: KTPAMXX can correctly look up message IDs like `TextMsg`, `SayText`, etc. without creating duplicates

### Technical Details

#### Hook Chain Additions to `IRehldsHookchains`
```cpp
virtual IRehldsHookRegistry_PF_changelevel_I* PF_changelevel_I() = 0;
virtual IRehldsHookRegistry_PF_setmodel_I* PF_setmodel_I() = 0;
virtual IRehldsHookRegistry_SV_ClientUserInfoChanged* SV_ClientUserInfoChanged() = 0;
virtual IRehldsHookRegistry_PF_RegUserMsg_I* PF_RegUserMsg_I() = 0;
virtual IRehldsHookRegistry_SV_ClientCommand* SV_ClientCommand() = 0;
virtual IRehldsHookRegistry_SV_InactivateClients* SV_InactivateClients() = 0;
```

#### Map Change Sequence Support
The `SV_InactivateClients` hook enables KTPAMXX to handle map changes properly:
1. `SV_InactivateClients()` → KTPAMXX fires `plugin_end`, `client_disconnect`
2. `SV_ServerShutdown()` → Server shuts down
3. `SV_SpawnServer()` → New map loads
4. `SV_ActivateServer()` → KTPAMXX fires `plugin_init`, `plugin_cfg`
5. `SV_Spawn_f()` → Clients reconnect, KTPAMXX fires `client_connect`, `client_putinserver`

### Compatibility Notes

- **Requires KTPAMXX 2.1.0+** for extension mode features
- **Backwards compatible** with standard ReAPI/AMXX plugins
- **API version unchanged** - existing extensions work without recompilation

---

## [KTP-ReHLDS `3.15.0.891-dev+m`] - 2025-12-02

**Bug Fix Release** - Critical stability and reliability improvements to pause system.

### Fixed
- **[CRITICAL] Race condition in pause state management** (`sv_main.cpp:8140-8180`)
  - **Issue**: Mid-frame race condition when plugin calls `SetServerPause()` from another thread
  - **Problem**: Checking `g_ktp_temporary_unpause` flag AFTER frame processing allowed plugins to modify it between setting and checking, causing pause state corruption
  - **Fix**: Store pause restore decision (`shouldRestorePause`) at frame start before any processing
  - **Impact**: Prevents pause state corruption where game stays unpaused when it should be paused
  - **Result**: Thread-safe pause state restoration

- **[CRITICAL] Network message flooding during pause** (`sv_main.cpp:5236-5243`)
  - **Issue**: Temporary unpause bypassed message timing checks entirely, flooding clients
  - **Problem**: Old logic sent messages EVERY frame during pause (60-100 Hz), overwhelming clients at high tick rates
  - **Fix**: Respect normal message timing even during temporary unpause while still allowing messages through
  - **Impact**: Prevents client message buffer overflow and network congestion
  - **Result**: Controlled message flow during pause without flooding

### Changed
- **Version number consistency**: Added `VERSION_BUILD 888` to `version.h`
  - Now matches CHANGELOG version format: `3.15.0.888-dev+m`

### Documentation
- Updated README.md version references from `3.14-ktp` to `3.15.0.888-dev+m`
- Verified all implemented functionality is documented in README

### Build Status
- ✅ **Windows**: MSBuild (Visual Studio 2022) - 3/3 components successful
- ✅ **Linux**: GCC 13.3.0 via WSL - All components successful (100%)
- ✅ **0 errors, minor warnings only**

### Technical Notes
- Race condition fix uses local variable to capture restore decision before frame processing
- Network throttle fix changes from bypass (`if (flag) send`) to controlled allow (`if (flag OR timing) send`)
- Both fixes maintain backward compatibility with existing plugin behavior

---

## [KTP-ReHLDS `3.15.0.888-dev+m`] - 2025-11-22

**KTP-ReHLDS** is a custom fork of ReHLDS with enhanced pause functionality for competitive gameplay.

### Added
- **Chat During Pause**: Partial chat functionality while game is paused
  - Engine rate limiting bypassed during temporary unpause frames
  - Commands processed correctly (e.g., `/cancel` works during pause)
  - First chat message displays to clients
  - **Known Limitation**: Subsequent chat messages blocked by DoD game DLL flood protection
    - Only first message per pause works for client "say" commands
    - RCON messages work normally as workaround
- **HUD Updates During Pause**: `RH_SV_UpdatePausedHUD` hook called every frame
  - Enables real-time countdown timers (MM:SS format)
  - Live pause info updates (extensions, commands, budget)
  - Auto-warnings at 30s and 10s remaining
- **Temporary Unpause System**: Frame-by-frame pause state manipulation
  - `g_ktp_temporary_unpause` flag distinguishes temporary vs permanent unpause
  - `g_psv.paused` temporarily cleared for chat/command processing
  - Physics remain frozen (`SV_Physics()` skipped when `shouldSimulate == FALSE`)
  - Game time (`g_psv.time`) remains frozen - match timer unaffected
  - Network messages sent/received normally during pause
  - State restored after `SV_SendClientMessages()` completes

### Fixed
- **ReHLDS String Command Rate Limiter**: Bypassed during temporary unpause
  - Issue: Rate limiter used `realtime` which continues during pause, causing false positives
  - Fix: Skip `g_StringCommandsRateLimiter.StringCommandIssued()` when `g_ktp_temporary_unpause == 1`
  - Result: Commands no longer blocked by engine rate limiting during pause

### Technical Details
- `realtime` continues advancing during pause (network timing)
- `g_psv.time` frozen during pause (game simulation time)
- `host_frametime` set to 0 during pause (physics freeze)
- `shouldSimulate` checked BEFORE pause manipulation (correct freeze behavior)
- Compatible with `pausable 0` (ReAPI native pause control)

### Repository
- **GitHub**: https://github.com/afraznein/KTP-ReHLDS
- **Based on**: ReHLDS 3.14.0.857 (upstream)

---

## [`3.14.0.857`](https://github.com/rehlds/rehlds/releases/tag/3.14.0.857) - 2025-03-27

### Added
- Added cvarhook from latest HLDS build. Making `mapcyclefile`/`sv_cheats` work in realtime (resolve #868) by @s1lentq in https://github.com/rehlds/ReHLDS/commit/6f031901cfa85d86c6028086bc9335db1e867a03
- Added forgotten CVar `sys_timescale` by @s1lentq  
- Add support cheats commands: `god`, `notarget`, `noclip` by @s1lentq (https://github.com/rehlds/ReHLDS/commit/7fcec97af4c6598ab6a65d60cf424b7775a11729);
- engine: add `sv_allow_autoaim` cvar for `HL25` DLL compatibility by @a1batross in #1000;
- Added new CVar: `r_cachestudio` by @s1lentq in https://github.com/rehlds/ReHLDS/commit/0af97d98bbe31af26395ba616d4be6a089f691b6;
- Implement commands `rcon_adduser`, `rcon_deluser`, `rcon_users` to allow use RCON only by known user IPs (Resolves https://github.com/rehlds/ReHLDS/pull/796)
SV_Rcon: Minor refactoring by @s1lentq in https://github.com/rehlds/ReHLDS/commit/62407e0dd63542f0da8438ebb22b33d3bcab3b9d;
- Implemented optional CVar `sv_tags` for sets a string defining the "gametags" for this server to allows users/scripts to filter in the matchmaking/server-browser interfaces based on the value by @s1lentq in https://github.com/rehlds/ReHLDS/commit/76cbd2c14025b3e54bc14b8fcfe17ce5fa273195;
- Add `SV_SendResources` hook by @ShadowsAdi in #1024;
- Implement API interface game message manager by @s1lentq in https://github.com/rehlds/ReHLDS/commit/c9f9bbfff98cdb6e8ca45b9ab626f40d0c7bb22b;
- CalcSurfaceExtents: MAX_SURFACE_TEXTURE_SIZE limit increased from 256 to 512 by @s1lentq in https://github.com/rehlds/ReHLDS/commit/b29740c19e8cfe7a1ee0605cadccd05b35e8678e;
- Added new CVars for improved handling of decompression failures by @s1lentq in https://github.com/rehlds/ReHLDS/commit/64c684af4a8e6030b9e06a59a774d6efd92db352;
- Added codesign and resources by @stamepicmorg in https://github.com/rehlds/ReHLDS/pull/1069;


### Fixed
- Fixed reversing mistake, missing checking string for null by @s1lentq in https://github.com/rehlds/ReHLDS/commit/2ba27d409c364d12c01b6b72813ac4b991a6e224;
- Host_Motd_f: Fixed viewing motd when motdfile is not specified by @s1lentq in https://github.com/rehlds/ReHLDS/commit/de3679f0391f1452532c820f07a8c4042b1c4281;
- FIX: Don't exec config file when exceed limit text buffer by @s1lentq in https://github.com/rehlds/ReHLDS/commit/32857e77859789275daeb353a278cd6632c2b6bf;
- Prevent crash `Cache_UnlinkLRU: NULL link` on client-side if aiment with sprite model will be to render as a studio model by @s1lentq in https://github.com/rehlds/ReHLDS/commit/5002ff9abe86e9573952e2dc45ec9229d0044a3b;
- RCON: Fixes redirect print and minor refactoring by @s1lentq in https://github.com/rehlds/ReHLDS/commit/41c5186b2c8c00f49101ed2c5d81a47cf79449c4;
- Host_Status_f: Fixed incorrect player index to output by @s1lentq in https://github.com/rehlds/ReHLDS/commit/9b0dbe8dd2ced5f0ead251116f5b9ebd61f6d6a4;
- MSG_WriteBitAngle: Cap the precision check from 32 to 22 to avoid overflow issues when representing angles with more than 22 bits because the multiply by 'shift' may result in overflow by @s1lentq in https://github.com/rehlds/ReHLDS/commit/63fde229c98f7e1bb8f7ea9b8358027414ed26ac;
- Fix reversing mistake in `TEX_InitFromWad` (Don't add file handle before check) by @s1lentq in https://github.com/rehlds/ReHLDS/commit/a7b60451f33e076ba04e4ae38312c2f691540a2d
- Fix crash when the entity with aiment doesn't have a model by @s1lentq in https://github.com/rehlds/ReHLDS/commit/498d7e0d18060f53a65286fcbf91e772f42c5611;
- SV_WriteEntitiesToClient: Reset movetype if the aiment index is invalid by @s1lentq in https://github.com/rehlds/ReHLDS/commit/58391b6ee5e5faefcf84349443a2a36066ed8246;
- SV_ParseResourceList: Do not uploading according to `sv_allowupload` CVar by @s1lentq in https://github.com/rehlds/ReHLDS/commit/59ed3f6867b1b3cba69cd7650887841da8e76d42;
- Do not send customizations list on duplicate or missing resource by @s1lentq in https://github.com/rehlds/ReHLDS/commit/f26ad71aba6a596602245c64af71cd196196859f;
- Do not propagate custom logos according to sv_send_logos cvar by @s1lentq in https://github.com/rehlds/ReHLDS/commit/ec47e4d97834c35f8667684f15a372c85505ce55;
- Draw_ValidateCustomLogo: Fixed incorrect offset to palette size by @s1lentq in https://github.com/rehlds/ReHLDS/commit/3c282b435c7d0508d72104d26a0c9171f7feea71;
- Netchan_CreateFileFragments: Fixed a hang connection on verifying resource stage, when precached file exists but is absolutely empty by @s1lentq in https://github.com/rehlds/ReHLDS/commit/61ee4f926938b1894bedfd9f139d45db48d89903;
- Implemented reduction of impact caused by zip-bomb exploit by @s1lentq in https://github.com/rehlds/ReHLDS/pull/994;
- Improved behavior of `sv_filterban 0`. Fixes https://github.com/rehlds/ReHLDS/issues/1027 by @s1lentq in https://github.com/rehlds/ReHLDS/commit/693b51c8839847270048be40b62fb1d37f9954ae;
- Fixed GCC compilation warnings/errors. Fixes https://github.com/rehlds/ReHLDS/issues/1032 by @s1lentq in https://github.com/rehlds/ReHLDS/commit/9c1e84328ebcdb47ea83c157dd8c7e493dda28a6;
- [HLTV]: Fix reverse-engineering mistake in `ObjectDictionary::RemoveIndex` by @s1lentq in https://github.com/rehlds/ReHLDS/commit/ed954a710fb0888328db1bd9b2c916cf9fc63062;
- [HLTV]: Fix reverse-engineering mistake in `World::WritePacketEntities` by @s1lentq in https://github.com/rehlds/ReHLDS/commit/c8308a2c60d34e4f735b3404f86701109e85f73b;
- fix setting ucmd in `sv_user.cpp` related to https://github.com/rehlds/ReHLDS/issues/1041 by @overl4y in https://github.com/rehlds/ReHLDS/pull/1042;
- SV_ProcessFile: Ignore customization file uploads if upload is disabled by @s1lentq in https://github.com/rehlds/ReHLDS/commit/fe184a82e0a361b9e8bd1625ca93fac948467be1;
- CI Workflow Improvements and Fixes by @SergeyShorokhov in #1056;
- Improved movevars sync logic for clients, allowing independent sync of movement props for each client, regardless of global movevars by @s1lentq in https://github.com/rehlds/ReHLDS/commit/df862d9bb6069084ee17b23135152c57b748e40e;
- FIX: potential crash in `PrecacheModelSounds` by @s1lentq in https://github.com/rehlds/ReHLDS/commit/18b173d5c6715eaf052a979984622f867661cd7b;
- FIX: crash due fakeclient by @s1lentq in https://github.com/rehlds/ReHLDS/commit/e54adb089c816425bac43c7eaf6fc5b898401fb4;
- Netchan_CopyFileFragments: fix typo by @s1lentq in https://github.com/rehlds/ReHLDS/commit/1a684077109023f86cfe9b70d6a1bba92a35c5f6;


### Changed
- Improve `pfnShouldCollide` condition on `SV_ClipToLinks` by @dystopm in #985
- Minor refactor (add `BoundsIntersect` function) by @Hamdi #986
- Reworked AlertMessage by @s1lentq in https://github.com/rehlds/ReHLDS/commit/93f5775ac26240782981f47ee8e052fb53d30877
- CI/CD update:
- Draw_ValidateCustomLogo: Minor refactoring & cleanup by @s1lentq in https://github.com/rehlds/ReHLDS/commit/174414db81116b3647e7bb1b906417d6a38ed3a3;
- HPAK_ResourceForHash: Remove message with missing custom.hpk by @s1lentq in https://github.com/rehlds/ReHLDS/commit/516bb936271b1cf8523d6f97a421370128a7c964;
- SV_CreateCustomizationList: spew logs in only dev mode by @s1lentq in https://github.com/rehlds/ReHLDS/commit/462fe55fb832209270118b6def6034f8eec6efbf;
- CalcSurfaceExtents: more info in extents error message by @s1lentq in https://github.com/rehlds/ReHLDS/commit/6e6368da300f7d024076e6272faa135280cf3bda;
- Move SV_CheckMovingGround into SV_Physics by @dystopm in #1045;
- `IP` and `IPX` allocation warnings move under `-dev` arg by @SergeyShorokhov in #1071;

## New Contributors
* @dystopm made their first contribution in https://github.com/rehlds/ReHLDS/pull/985
* @anzz1 made their first contribution in https://github.com/rehlds/ReHLDS/pull/1021
* @jonathan-up made their first contribution in https://github.com/rehlds/ReHLDS/pull/1040
* @overl4y made their first contribution in https://github.com/rehlds/ReHLDS/pull/1042
* @stamepicmorg made their first contribution in https://github.com/rehlds/ReHLDS/pull/1058

**Full Changelog**: [3.13.0.788...3.14.0.857](https://github.com/rehlds/rehlds/compare/3.13.0.788...3.14.0.857)

## [`3.13.0.788`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.13.0.788) - 2023-07-12

### Added
- Added `SV_AllowPhysent` hook by @justgo97 in [(#951)](ttps://github.com/dreamstalker/ReHLDS/pull/951)
- `GetBonePosition`: Added bone index bounds check
- `GetAttachment`: Added attachment index bounds check
- Added more checks for possible `numleaf` overflow

### Fixed
- `SV_BuildSoundMsg`: fix '`\n`' in args check

### Changed
- Revert "change destinition folder for linux build" by @wopox1337 in [(#977)](https://github.com/dreamstalker/ReHLDS/pull/977)
- Allowed the clients to connect on the server of different game: Client should be use `setinfo _gd <game>`
- Increased limit leafs `MAX_MAP_LEAFS` up to `32767`

## New Contributors
- @justgo97 made their first contribution in [(#951)](https://github.com/dreamstalker/ReHLDS/pull/951)

**Full Changelog**: [3.12.0.780...3.13.0.788](https://github.com/ReHLDS/ReHLDS/compare/3.12.0.780...3.13.0.788)

## [`3.12.0.780`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.12.0.780) - 2022-09-19

### Fixed
- `Netchan_CreateFileFragments`: Fixed a very old and rare bug with dlfile while downloading direct from server, when content of resource size is less than header size first fragment.

### Changed
- `API`: Implement `*_Precache_*`, `ClientPrintf`, `CheckUserInfo` and `AddResource` hooks by @ShadowsAdi in [(#903)](https://github.com/dreamstalker/ReHLDS/pull/903)

## New Contributors
* @ShadowsAdi made their first contribution in [(#903)](https://github.com/dreamstalker/ReHLDS/pull/903)

**Full Changelog**: [3.11.0.779...3.12.0.780](https://github.com/ReHLDS/ReHLDS/compare/3.11.0.779...3.12.0.780)

## [`3.11.0.779`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.11.0.779) - 2022-08-24

### Fixed
- `StripUnprintableWorker` did not count the null terminator [e9045e3](https://github.com/dreamstalker/ReHLDS/commit/e9045e3)
- Very old and rare bug in function `Netchan_CreateFileFragments` with dlfile hang while downloading direct from server, when content of resource size is less than header size first fragment [d76b06d](https://github.com/dreamstalker/ReHLDS/commit/d76b06d)

**Full Changelog**: [3.11.0.777...3.11.0.779](https://github.com/ReHLDS/ReHLDS/compare/3.11.0.777...3.11.0.779)

## [`3.11.0.777`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.11.0.777) - 2022-06-20

### Fixed
* Fixed `null or empty` input string in `COM_LoadFile` (`FS_Open` with input empty string `""` will succeed on some POSIX systems)
 - Resolved  [(#919)](https://github.com/dreamstalker/ReHLDS/issues/919)

**Full Changelog**: [3.11.0.776...3.11.0.777](https://github.com/ReHLDS/ReHLDS/compare/3.11.0.776...3.11.0.777)

## [`3.11.0.776`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.11.0.776) - 2022-04-20

### Fixed
* Fixed typo `ZONE_DYNAMIC_SIZE`

**Full Changelog**: [3.11.0.767...3.11.0.776](https://github.com/ReHLDS/ReHLDS/compare/3.11.0.767...3.11.0.776)

## [`3.11.0.767`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.11.0.767) - 2021-10-25

### Added
* Implement `SV_EmitPings()` hook by @francoromaniello in [(#858)](https://github.com/ReHLDS/ReHLDS/pull/858)
* Implement `Con_Printf()` hook by @francoromaniello in [(#861)](https://github.com/ReHLDS/ReHLDS/pull/861)

### Changed
* `API`: Add hooks `ED_Alloc()` & `ED_Free()`. by @StevenKal in [(#867)](https://github.com/ReHLDS/ReHLDS/pull/867)
* `SV_HullForEntity`: better log in `Sys_Error` by @wopox1337 in [(#843)](https://github.com/ReHLDS/ReHLDS/pull/843)
* Update on grammar/spelling by @mlgpero in [(#865)](https://github.com/ReHLDS/ReHLDS/pull/865)

## New Contributors
* @StevenKal made their first contribution in [(#867)](https://github.com/ReHLDS/ReHLDS/pull/867)
* @francoromaniello made their first contribution in [(#858)](https://github.com/ReHLDS/ReHLDS/pull/858)
* @Urufusan made their first contribution in [(#865)](https://github.com/ReHLDS/ReHLDS/pull/865)

**Full Changelog**: [v3.10.0.761...3.11.0.767](https://github.com/ReHLDS/ReHLDS/compare/v3.10.0.761...3.11.0.767)

## [`3.10.0.760`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.10.0.760) - 2021-06-23

### Changed
- Changed the destination folder for `Linux build` [(#842)](https://github.com/ReHLDS/ReHLDS/pull/842).
- Temporary removed `Windows build`. :warning:

**Full Changelog**: [3.10.0.759...3.10.0.760](https://github.com/ReHLDS/ReHLDS/compare/3.10.0.759...3.10.0.760)


## [`3.10.0.761`](https://github.com/ReHLDS/ReHLDS/releases/tag/v3.10.0.761) - 2021-06-23

### Changed
- Reset `m_bSentNewResponse` to allow new connection when the client goes through the full stage of connection (`cl:connect` -> `sv:S2C_CONNECTION` -> `cl:new` -> `SV_New_f`) 
  - Related [3a9bfb9](https://github.com/ReHLDS/ReHLDS/commit/3a9bfb9)

**Full Changelog**: [3.10.0.760...v3.10.0.761](https://github.com/ReHLDS/ReHLDS/compare/3.10.0.760...v3.10.0.761)

## [`3.10.0.760`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.10.0.760) - 2021-06-23

### Changed
- Changed the destination folder for `Linux build` [(#842)](https://github.com/ReHLDS/ReHLDS/pull/842).
- Temporary removed `Windows build`. :warning:

**Full Changelog**: [3.10.0.759...3.10.0.760](https://github.com/ReHLDS/ReHLDS/compare/3.10.0.759...3.10.0.760)

## [`3.10.0.759`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.10.0.759) - 2021-06-22

### Fixed
- Fixed volume checking in emit sound [(#341)](https://github.com/ReHLDS/ReHLDS/pull/341)
- `static_map.h`: fix lowercase convert [(#806)](https://github.com/ReHLDS/ReHLDS/pull/806)
- `SV_New_f`: Deny new connection twice at a time if user messages are received;
  - `SV_ReadClientMessage`: Fixed empty names on bad read.

### Changed
- `sv_user.cpp`: Small code refactoring [(#810)](https://github.com/ReHLDS/ReHLDS/pull/810)
- `ReHLDS API`: Enhanced IGameClient/IRehldsServerData/IRehldsServerStatic interfaces
- `sv_main.cpp`: SV_New_f() uses Q_snprintf() unsafe format. #807 [()](https://github.com/ReHLDS/ReHLDS/pull/807)

**Full Changelog**: [3.9.0.752...3.10.0.759](https://github.com/ReHLDS/ReHLDS/compare/3.9.0.752...3.10.0.759)

## [`3.9.0.752`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.9.0.752) - 2021-06-14

### Added
- `ReHLDS API`: Add GetEntityInit hook [(#832)](https://github.com/ReHLDS/ReHLDS/pull/832)
- Implement CVar `sv_usercmd_custom_random_seed` [(#837)](https://github.com/ReHLDS/ReHLDS/pull/837)

### Fixed
- `HLTV`: Fix crash in ProcessStringCmd [(#838)](https://github.com/ReHLDS/ReHLDS/pull/838)

### Changed
- `SV_ParseMove`, `SV_ParseConsistencyResponse`: check length

**Full Changelog**: [3.8.0.739...3.9.0.752](https://github.com/ReHLDS/ReHLDS/compare/3.8.0.739...3.9.0.752)

## [`3.8.0.739`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.8.0.739) - 2021-04-21

### Added
* Added libraries libm/librt built on `GLIBC` `2.11.1`  [(#827)](https://github.com/ReHLDS/ReHLDS/pull/827)

### Fixed
* `QuaternionSlerp`: Fixed wrong values  [(#763)](https://github.com/ReHLDS/ReHLDS/pull/763)

### Changed
* Updated `Intel C++ Compiler` version `17.0` up to `19.0`

**Full Changelog**: [3.8.0.723...3.8.0.739](https://github.com/ReHLDS/ReHLDS/compare/3.8.0.723...3.8.0.739)

## [`3.8.0.723`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.8.0.723) - 2021-03-23

### Fixed
* `CalcSurfaceExtents:` Fixed a fatal error on some maps due loss of floating point
* `HLTV:` ExecuteString Fix parser  [(#821)](https://github.com/ReHLDS/ReHLDS/pull/821)

### Changed
* `HLTV:` Downgrade GLIBC version

**Full Changelog**: [3.8.0.715...3.8.0.723](https://github.com/ReHLDS/ReHLDS/compare/3.8.0.715...3.8.0.723)

## [`3.8.0.715`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.8.0.715) - 2021-03-18

### Fixed
* Make sure the `timer` is high precision  [(#799)](https://github.com/ReHLDS/ReHLDS/pull/799)

**Full Changelog**: [3.8.0.711...3.8.0.715](https://github.com/ReHLDS/ReHLDS/compare/3.8.0.711...3.8.0.715)

## [`3.8.0.711`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.8.0.711) - 2021-02-06

### Added
* `HLTV`: Added new chatdelay command  [(#777)](https://github.com/ReHLDS/ReHLDS/pull/777)
* `HLTV`: prevent clients from setting userinfo * keys with setinfo command  [(#792)](https://github.com/ReHLDS/ReHLDS/pull/792)
* `Cbuf_Execute`: Add checks commented out for multi-lines  [(#719)](https://github.com/ReHLDS/ReHLDS/pull/719)

### Fixed
* Fixed local-buffer overrun, may undefined behavior with hitboxes blending or crash (reverse-engineering mistake) [722e19d](https://github.com/ReHLDS/ReHLDS/commit/722e19d)
* Fixed dos attack on connection challenges system  [(#802)](https://github.com/ReHLDS/ReHLDS/pull/802)
* Fixed crash `COM_ListMaps`  [(#791)](https://github.com/ReHLDS/ReHLDS/pull/791)

**Full Changelog**: [3.8.0.702...3.8.0.711](https://github.com/ReHLDS/ReHLDS/compare/3.8.0.702...3.8.0.711)

## [`3.8.0.702`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.8.0.702) - 2020-11-09

### Fixed
* Fixed crash `MSG_ReadFloat`

### Changed
* **ReHLDS API:** Implemented `SV_ShouldSendConsistencyList`
* **ReHLDS API:** Bump minor

**Full Changelog**: [3.7.0.698...3.8.0.702](https://github.com/ReHLDS/ReHLDS/compare/3.7.0.698...3.8.0.702)

## [`3.7.0.698`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.7.0.698) - 2020-08-19

### Added
* Graceful shutdown on sigterm  [(#776)](https://github.com/ReHLDS/ReHLDS/pull/776)
  - Add players kick on `SIGINT \ SIGTERM`
  - Add SIGINT & SIGTERM handling linux console

### Changed
* Changed shutdown method

**Full Changelog**: [3.7.0.697...3.7.0.698](https://github.com/ReHLDS/ReHLDS/compare/3.7.0.697...3.7.0.698)

## [`3.7.0.697`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.7.0.697) - 2020-08-10

### Fixed
* **SV_ProcessFile:** Wrap `Con_Printf` in `Con_NetPrintf` to avoid spam in HLDS console

**Full Changelog**: [3.7.0.696...3.7.0.697](https://github.com/ReHLDS/ReHLDS/compare/3.7.0.696...3.7.0.697)

## [`3.7.0.696`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.7.0.696) - 2020-05-18

### Added
* Implement `svc_exec` support in the engine and HLTV [(#737)](https://github.com/ReHLDS/ReHLDS/pull/737)
  - Added `svc_exec` to the list of svc commands in engine
  - Added `svc_exec` support to HLTV code
  - Made the engine code forward-compatible with future svc_* additions
  - Added reserved svc_* slots in the enumerations

**Full Changelog**: [3.7.0.695...3.7.0.696](https://github.com/ReHLDS/ReHLDS/compare/3.7.0.695...3.7.0.696)

## [`3.7.0.695`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.7.0.695) - 2020-04-06

### Fixed
* Vulnerability fix WAD part 2
  - Client-side: Fixed a potential vulnerability from bogus `tempdecal.wad`

**Full Changelog**: [3.7.0.694...3.7.0.695](https://github.com/ReHLDS/ReHLDS/compare/3.7.0.694...3.7.0.695)

## [`3.7.0.694`](https://github.com/ReHLDS/ReHLDS/releases/tag/3.7.0.694) - 2020-03-20

### Fixed
* Vulnerability fix WAD part 1
  - Server-side: Fixed a potential vulnerability from bogus `tempdecal.wad`

**Full Changelog**: [3.7.0.694](https://github.com/ReHLDS/ReHLDS/commits/3.7.0.694)
