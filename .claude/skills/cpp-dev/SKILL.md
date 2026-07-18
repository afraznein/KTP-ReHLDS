---
name: cpp-dev
description: Use BEFORE modifying any KTPReHLDS C++ (engine, dedicated launcher, rehlds_api) — fork-delta discipline, the cross-thread Con_Printf/telemetry traps, the vtable-ABI rule, build/smoke-test/stage workflow, and verify-by-md5. Also use when planning a change, to know which invariants it touches.
---

# KTPReHLDS Development

This is a fork of ReHLDS (repo root nests one level down at `KTPReHLDS/rehlds`,
remote `upstream` = dreamstalker/rehlds) running as the game engine for a
24-instance production Day of Defeat fleet, with KTPAMXX loaded directly as an
extension (no Metamod). The rules below each encode a real production incident
or a confirmed live bug; follow them even when they feel paranoid.

## Hard safety rules
- **NEVER restart game servers** without the operator's explicit permission in
  the current conversation — this repo is the engine binary itself, so a bad
  deploy takes down every instance on a host.
- Binaries deploy as `.new` files (`engine_i486.so.new`, `hlds_linux.new`) and
  swap at the 03:00 ET nightly restart. `chmod +x` after upload; a `.new` that
  isn't executable fails silently at swap time.
- **Verify deploys by md5, never by the console banner.** `appversion` is a
  commit-count that skews against CHANGELOG version titles — two different
  builds can show the same banner, and the same build can show different
  banners after an unrelated commit lands.

## Fork-delta discipline
Only touch KTP-owned code (marked with `KTP` comments, or findable via
`git diff upstream/master..main -w --ignore-cr-at-eol` — raw diff stats on
files like sv_main.cpp/sv_user.cpp/rehlds_api_impl.* are inflated ~10x by CRLF
rewrites, so always diff whitespace-ignoring). Upstream code is out of scope
for cleanup, refactor, or "optimization" — changing it breaks merge-ability.
When a fix genuinely requires editing an upstream file, keep the diff minimal
and say so in the CHANGELOG entry.

## Cross-thread hazards (the most important section in this file)
This engine has grown three threads beyond the main game thread: the .927
async log writer, the Steam background thread (`sv_steam3.cpp`, since .913),
and the Linux `-netthread` receive thread. Every one of them has produced a
confirmed unsynchronized-race bug reachable from ordinary error paths:
- `Con_Printf`/`Con_DebugLog` are **not** thread-safe (unsynchronized rcon
  redirect buffer `outputbuf`, unsynchronized `g_ktp_conio_*` profiling
  doubles) but both the Steam thread and the net thread can reach them on a
  `sendto()` error path. Never let new background-thread code call
  `Con_Printf` directly — route diagnostics through the async log ring, or
  gate on a thread-identity check.
- Any global mutated from more than one thread needs an atomic or a mutex,
  full stop — `NET_SendLong`'s static `gSequenceNumber` became cross-thread
  reachable the moment Steam sends moved off the main thread, with no atomic
  added. Before moving any `NET_SendPacket`-calling code onto a new thread,
  grep everything it touches for other callers.
- **New background threads must use `pthread_create` with a checked return,
  never `std::thread`/`new std::thread`.** This engine builds `-fno-exceptions`;
  `std::thread`'s constructor throws on resource exhaustion, which aborts the
  process instead of degrading. The .927 log writer and the `-netthread` port
  do this correctly; `sv_steam3.cpp`'s thread creation (predates the
  convention) does not — don't copy that call site.
- `-netthread` shutdown is a known hang: `NET_Shutdown` closes sockets before
  stopping the thread, and the thread's `select()` has no bounded timeout by
  default, so it never re-polls the stop flag. Opt-in only today (fleet runs
  `-pingboost 2` without it) — fix this before ever recommending `-netthread`.

## Other confirmed live invariants
- **`IRehldsHookchains` vtable changes must bump `REHLDS_API_VERSION`.**
  The fork has already inserted a KTP virtual mid-vtable without a version
  bump — version negotiation currently cannot tell an ABI-incompatible pairing
  apart from a compatible one. New virtuals: append only, and bump the version
  when you do. KTPReAPI and KTPAMXX must rebuild in the same wave as any
  interface change.
- **`SV_ClientUserInfoChanged` is LIVE again as of `.929`, behind `ktp_userinfo_hook`
  (default 1).** It had been commented out since 2025-12-06 ("investigating client
  kick issue"); it turned out the hook was added **already disabled**, in the very
  commit that introduced it, and never executed on any branch — so that comment
  recorded a dev-time observation, not a production incident. Two things to preserve:
  - **The call site is an `if/else` and must stay one.** The chain's terminal
    (`SV_ClientUserInfoChanged_internal`) calls `pfnClientUserInfoChanged` itself,
    and `callChain` runs the terminal even with zero hooks registered — so adding
    the hook call *alongside* the direct call dispatches every name change to the
    game DLL **twice**. That is the most economical explanation for the original
    kick. Never reintroduce the direct call as a second statement.
  - `ktp_userinfo_hook 0` is the live kill switch (read per userinfo update, no
    latch, no restart); the `0` branch is byte-for-byte the pre-.929 path.
  - **Ask "was this ever live?" with `git log --all -G`, not `-S`.** `-S` counts
    occurrence-count *changes* and silently misses whole-file rewrites that preserve
    the string — it found only 1 of the 2 commits touching this call site.
- **The `addip`/`banid` command blocks don't distinguish a human rcon caller
  from an engine-internal auto-ban caller.** `SV_AddIP_f`/`SV_BanId_f` reject
  everything unconditionally, which also silently defangs the engine's own
  rcon-brute-force ban, flood-punish bans, and decompression-punish bans while
  the console still logs "Banning...". Any future audit-gate change here needs
  an internal bypass path, not a blanket reject.
- **`[KTP_PROFILE]` interval telemetry has a recurring bad pattern: sampling
  the boundary frame instead of tracking a running peak across the interval.**
  Check any new `_detail` field's reset point — per-frame reset samples only
  the frame that happens to cross the log boundary; per-interval reset (with
  its own peak var) is correct.
- The intrusive hookchain-registry list (`hookchains_impl.cpp`) **now unlinks on
  destruction as of `.929`** — `MessageManagerImpl` `new`s/`delete`s message-hook
  registries at runtime, and a freed node left linked was a heap use-after-free for
  `.928`'s `KTP_ClearAllHooks` to memset at shutdown. The dtor is **`protected`**, so
  an accidental `delete` through the base is a compile error rather than UB; keep it
  that way. Registries are destroyed only through their concrete type.
- **Pause nodelta (`s_ktp_pauseTransitionFrames`) is scaffolding that guards nothing
  — slated for REMOVAL, do not "fix" or extend it.** The `.929` review proved from
  source that a pause cannot invalidate a client's delta baseline: selection is
  purely ack-driven (`fromframe = &client->frames[SV_UPDATE_MASK & client->delta_sequence]`)
  and the per-client frame ring advances only on sends to that client, while sends and
  acks keep flowing during pause (entities are merely frozen). `.914` narrowing it from
  every-frame to a 3-frame window removed nothing load-bearing. A per-client version was
  written, reviewed, and dropped for exactly this reason. **Two traps if you ever touch
  it:** `client_t` is ABI-exposed to the game DLL and ReAPI (`IGameClient`) so per-client
  state must be a parallel array, never a new field; and `client->delta_sequence` is the
  **client's ack**, rewritten from its next move packet — stomping it to -1 to force a
  flush gets clobbered before it emits.

## Async log writer pattern (v3.22.0.927+)
Game thread enqueues into a bounded 2048-slot ring; full queue = drop and
increment a counter, **never block**. A dedicated writer thread owns the log
`FILE*` (line-buffered via `setvbuf` for crash durability) and does the actual
disk write off the game thread. Kill switch: `ktp_log_async` (default 1, `0`
= exact legacy synchronous path, latches at `Log_Open`). Follow this same
shape — bounded ring, drop-with-counter, `pthread_create`-with-sync-fallback —
for any new work that touches disk or network from a hot path. Con_DebugLog
(qconsole.log via `-condebug`) is a known exception the async writer does NOT
cover; watch `conprintf_worst=` on the `io:` line if that ever needs fixing.

## Never run a destructive simulation inside the working tree
Verifying a fix often means simulating the failure — writing a fake `build.sh`, a
fake artifact, a fake staging dir. Do it in a **verified** scratch dir, never in
the repo:

```bash
T="$(mktemp -d)" || exit 1
[ -n "$T" ] && [ -d "$T" ] || exit 1   # verify BEFORE you cd — this is the whole rule
cd "$T" || exit 1
```

`cd "$T"` with an empty `$T` **silently succeeds and leaves you where you were** —
in the repo. A simulation that then writes `build.sh` overwrites the real one. On
2026-07-16 exactly that truncated a tracked 60-line upstream file to 2 lines and
dropped a junk `.so` into `build/`, where a `find | head -1` could have staged it.
It was caught only because `git status` showed a modification nobody made.

So: verify the scratch dir before `cd`, and **run `git status` after any test that
touches the filesystem** — an unexpected change is the tell. Prefer copying inputs
out to the scratch dir over running tools "in place".

## Workflow
1. **Build**: `wsl bash -c "cd '/mnt/n/Nein_/KTP Git Projects/KTPReHLDS' && bash build_linux.sh"`
   (CMake → `rehlds/build/`, auto-stages to the KTP DoD Server test tree).
2. **Review**: `ktp-code-review` agent before staging anything nontrivial.
3. **Smoke test on the Tier-2 runner** (data server, `/opt/ktp-tier2-runner`):
   before/after comparison for crash-class fixes (boot → map change → quit,
   exit 0, ordered log rotation). The runner's module stack must match the
   fleet; sync it manually after any wave verifies — never automate the sync.
4. **Fleet stage**: `.new` via paramiko to all 24 active instances, `chmod +x`,
   md5-verify every staged file.
5. **Post-activation verify**: 24/24 on the new md5, no leftover `.new`, zero
   new cores — check `/tmp` (`find /tmp -maxdepth 1 -name 'core.*' -mtime -1`),
   NOT the game trees (that search matches only core.so/core.ini/core.wav and
   always looks clean whether or not anything crashed). Verify by md5, never
   the console banner (see Hard safety rules).

## Versioning
Bump the version for every shipped change and write the CHANGELOG.md entry
with what/why + the md5 of the shipped binary once built. Comments: short,
why-not-what, no ticket/finding IDs, never delete a tripwire fact while
trimming prose.
