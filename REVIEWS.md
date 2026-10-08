# Code review record

What has been read, against what range, by what method — and what was left unread.

This file exists because a commit subject saying *"review fixes"* is not evidence that a
review happened, and because this fork ships straight to a production fleet that keeps no
rollback copies. It records reading, not correctness.

**How to read an entry.** A range listed here was read; a range absent from it has **no
recorded review**, which is not the same claim as *unreviewed*. The "Not checked" section is
the load-bearing half — an entry without one is worth less than no entry, because it invites
a reader to assume coverage that was never measured.

**How to add one.** Newest first. Anchor on a commit range, never on a version label or a
banner: `appversion` is a commit count and two different builds can stamp the same one.
Keep the entries properties, not totals — a number in prose has no test holding it true.

---

## `d963701..9705614` — reviewed 2026-10-08

The `.931` engine cut: profiling and spike-attribution telemetry, the rcon-shutdown refusal,
the cross-thread work left over from moving Steam pumps onto a background thread, two
`HPAK_AddLump` allocation guards, and the build wrapper's staging gate.

**Method.** Read as a diff against `origin/main` (`git diff`/`git show` on the range, never
by opening files in a shared checkout). Each claim the commits make about *why* a change is
safe was re-derived from source rather than taken from the commit message or the CHANGELOG:

- Reachability of the deleted `g_ktp_temporary_unpause` branches, by tracing the flag's
  writers against the position of the `SV_ReadPackets` call inside `SV_Frame_Internal`, and
  by confirming `SV_ParseStringCommand` has no caller outside that subtree.
- The rcon-shutdown guard, by checking the declared type of `g_bRconCommand` against its
  definition, confirming its set and clear bracket a single straight-line call with no
  intervening early return, and mapping every console command name the README claims is
  refused onto the handler that is actually guarded.
- `[KTP_SPIKE_IO]`'s "worst sink, not the sum" gate, by confirming in `sv_log.cpp` that the
  `logaddr` and `file` spans are accumulated inside the `Log_Printf` span that feeds `logio`.
- The `HPAK_AddLump` null checks, by confirming `Mem_Malloc` is a bare `malloc` (so the
  branch is reachable, not dead), that `REHLDS_FIXES` is in the engine target's compile
  definitions (so the branch is compiled), and that each bail closes and frees exactly what
  the function's pre-existing sibling error paths do.
- The interval-peak `send_detail_peak` shadows, against the per-frame reset point of the
  values they shadow.
- The profiling re-enable reset, by confirming the first interframe sample really is
  suppressed — the zero it stores is already guarded at the read site.
- `build.sh`'s unconditional trailing `exit 0`, which is the premise the new staging gate
  rests on.

**Found.** No functional defect in the engine sources. Two documentation defects, fixed in
the same change as this entry: this repo's `cpp-dev` skill asserted three cross-thread
hazards as open that the range closes, and told the reader not to copy `sv_steam3.cpp`'s
thread creation, which is now the correct example; and `CLAUDE.md` described `build_linux.sh`
as gitignored and box-specific when it is tracked and its staging path is an override.

One cosmetic defect left in place: the third rejected-packet accounting block in
`SV_ReadPackets` is indented at the wrong level for its nesting. Behaviour is unaffected and
the file was being edited concurrently.

One pre-existing hazard the range narrows but does not close, now recorded in the skill:
gating the rcon redirect capture on thread identity sends an off-thread `Con_Printf` down the
`Con_DebugLog` branch, whose static path cache is unsynchronized and which writes through the
engine FS layer. That branch was already the off-thread destination whenever no redirect was
active, so this is the older exposure rather than a new one.

**Not checked.**

- **Nothing was built or run.** No compile was performed for this review and no artifact was
  produced. The repo's own CI compiled the range's tip green; it did not compile every
  intermediate commit, and a compile is not evidence that the engine loads or that a hookchain
  dispatches.
- **Nothing was read from a live host.** This review cannot say what any server is running,
  and the only identity that would answer that is an md5 of the deployed binary.
- **The Windows build was not exercised.** Several changes in the range are Windows-only
  (`clock_gettime` guards, `CreateThread`) and were read, not compiled.
- **`#ifndef REHLDS_FIXES` branches were not reviewed as shipping code.** `REHLDS_FIXES` is
  defined for the engine target, so those arms are not compiled here.
- **Numeric agreement between the telemetry and its downstream aggregator was not verified.**
  The CHANGELOG reports a row-level reconciliation; that figure was read, not reproduced.
- **Upstream ReHLDS code was read only where this range touches it.** The fork delta as a
  whole was not re-audited.
