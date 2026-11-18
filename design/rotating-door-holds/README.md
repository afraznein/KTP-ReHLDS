# Rotating-door holds: reproduction and design

Status: design for a ruling. No engine change ships from this document. Every proposal in it is
listed under "Decisions requested" so it can be tracked.

## Summary

- A long door hold is a rotating door that keeps pushing into one player, step after step, for
  seconds. In the `push:` telemetry it reads as `blocked_ms` close to the whole 10 s interval
  (4,930 of 5,000 possible 500 Hz steps in the worst intervals).
- The hold needs two things at once: a player the door cannot push clear (wedged against a wall, or
  caught by the engine's rotated-brush approximation), and a **reversal that does not reverse**. The
  second part is in the game DLL. When a closing door is blocked, DoD's `CBaseDoor::DoorGoUp` picks
  the side to reopen toward from **the facing of whoever last opened the door**, so for about half
  of all facings the "reopen" target lies past the blocker in the direction the door was already
  moving. The door keeps pressing into the player until that person turns or the blocker leaves.
- DoD's `CBaseDoor::Blocked` acts at most once every 0.25 s of server time, so the hold is set in
  game time. That is why `ktp_pusher_step_hz 500` did not change the long holds, and why the same
  hold reproduces at ticrate 100.
- Two fleet holds are reproduced below with a deterministic model on the real map hulls. In both,
  the model puts the blocker within 2.5 units of where the logs placed him during or after the
  hold.
  - `dod_thunder2`: a real wedge. True box collision still holds; fixing the reversal removes it.
  - `dod_armory_b7`: a phantom contact. With the engine's rotated-brush approximation, the hinge end
    of the leaf reaches a crouched player that true box collision never touches. Either fix removes
    this hold.
- So option C as carded (rotated-brush collision) would remove some long holds, not all of them.
  The change that removes both reproduced holds is a reversal guard. It is proposed below as the
  first engine candidate, with C as the second.

## How a hold shows up in telemetry

`[KTP_PROFILE] push:` is printed every 10 s. Since the 2026-11-16 engine, `blocked_ms` is the door
motion lost to blocks and reads the same at any step rate. Before that, `blocked` counted steps
(about 2.1 ms each at 500 Hz) or frames (about 1 ms each per frame).

- Era with `blocked_ms` (11-16 to 11-18, all instances): 111 non-zero intervals. Of the 49 that hold
  one episode on one door, 44 are one step (2–3 ms), meaning the first reversal worked. The longest
  single-episode intervals are 303 ms and 463 ms; the largest interval overall is 992 ms.
- Long holds in the 11-11 to 11-18 week, at 500 Hz: `dod_thunder2` 2,306 steps (about 4.85 s, cut
  short by a changelevel), `dod_armory_b7` 1,638 steps (about 3.45 s), `dod_harrington` 815 steps,
  `dod_lennon5_b1` 506 steps.
- Sessions on 2026-11-10 that look like deliberate testing (one victim, quick map changes) held
  doors on `dod_railroad2_s10b`, `dod_anzio` and `dod_lennon5_b1` for minutes at 4,920–4,950 steps
  per 10 s, one door at a time. A hold lasts as long as nobody moves.
- `push_detail:` names the pusher by edict index and classname and the victim by slot. Every long
  hold above names a `func_door_rotating`. `move=` (linear doors) is 0 in all of them.

## The mechanism, read from source and binary

### Engine (this repo, `rehlds/engine/sv_phys.cpp`)

`SV_PushRotate` turns the door one step. For each player now in solid (`SV_TestEntityPosition`,
which tests against the world and every entity, not just this door), it gives him the rigid
rotation about the hinge as one `SV_PushEntity` trace with the door non-solid. If he is still in
solid, it reverts the door step and the player and calls `pfnBlocked(door, player)`. Nothing in the
engine ever moves a blocked player out. `SV_SingleClipMoveToEntity` (`world.cpp`) and the server
`pmove` tests (`pmovetst.cpp`) both use the GoldSrc rotated-brush approximation: they rotate the
trace endpoints into the door's frame and test them against the door's pre-expanded axis-aligned
clip hull. In effect, the player's box rotates with the leaf. The error is zero at 0° and 90°
because the player footprint is square, and largest mid-swing.

### Game DLL (`dod/dlls/dod_i386.so`, md5 `4f4727b2390d3a0ed6f5ad862dd6d4be`, the file `liblist.gam` loads)

Decoded from the disassembly. The symbols are exported (`objdump -d -M intel --start-address=...`):

| Function | Address | What it does |
|---|---|---|
| `Blocked__9CBaseDoorP11CBaseEntity` | `0xbac0c` | Applies `dmg`, then `if (gpGlobals->time - this[0x230] < 0.25f) return;` (constant at `0x16843c`), stores the time, then reverses: `TS_GOING_DOWN` → `DoorGoUp`, otherwise `DoorGoDown`. **Not in the HLSDK.** The SDK reverses on every call. |
| `DoorGoUp__9CBaseDoor` | `0xba464` | Records `was_down = (m_toggle_state == TS_GOING_DOWN)`. For `func_door_rotating`: `sign = 1`. If `m_hActivator` is set, the door is not one-way and `movedir.y != 0`, it computes `sign = -1` when `vec.x*vnext.y - vec.y*vnext.x < 0`, where `vec` runs from the hinge to the activator and `vnext` from the hinge to the activator's origin plus 10 units along his facing (the SDK test). **DoD addition:** `if (was_down && angles.y < 0) sign = -sign`. Then `AngularMove(m_vecAngle2 * sign)`. |
| `DoorGoDown__9CBaseDoor` | `0xba8ec` | `AngularMove(m_vecAngle1)`: always toward closed. |
| `AngularMove__11CBaseToggleG6Vectorf` | `0x10c4f8` | Stores the destination in `this[0x1e8..0x1f0]` (`m_vecFinalAngle`), then sets `avelocity` and `nextthink`. `AngularMoveDone` snaps `angles` to it. |

`m_hActivator` is set in `DoorTouch` when a closed door is opened. Touch then stays off until the
door is fully closed again, so on every block during that cycle the swing side comes from the
original opener's facing **at the moment of the block**, wherever he is on the map.

DoD's flip reads like an attempt at "reopen toward the side you came from". With the facing term
forced to +1, that is exactly what it does. The facing term runs first, though, so the result
still depends on where the opener happens to be looking.

### Why a hold forms, and why only on rotating doors

A door closing onto a wedged player is blocked, and `Blocked` calls `DoorGoUp`. If the computed side
is the far one, the new target is past the player in the same rotational direction, so the next
step is blocked again. For 0.25 s every `Blocked` call is ignored. Then `Blocked` calls
`DoorGoDown`, which heads toward closed and so still into the player. After another 0.25 s it calls
`DoorGoUp` again and re-reads the facing. The door state alternates every 0.25 s while the angle
stays fixed. The model trace below shows exactly that. The hold ends at the first `DoorGoUp` that
reads a good facing, or when the player leaves.

A `func_door` has no side to pick: `DoorGoUp` is `LinearMove(m_vecPosition2)`, the true opposite of
`DoorGoDown`, so a blocked linear door always backs off. A blocked **opening** rotating door also
backs off, because `DoorGoDown` heads toward closed, which is the reverse of opening. Only a
blocked **closing** rotating door can fail to reverse.

None of this depends on the pusher step. The 0.25 s limit is server time, and the side choice
depends on positions and facing.

## Reproduction

### The model: `doorsim.py` (next to this file)

The model is a plain Python script with no dependencies. It reads the map's own BSP: planes,
clipnodes, the door's model hull, and the world hull (1 standing, 3 crouched). It runs the engine
pusher step and block path above, with the door test either the engine approximation (default) or
a true rotated-box test (`--exact`). It also runs DoD's door state machine as decoded, or the stock
SDK one (`--sdk`), or the proposed reversal rule (`--keep-side`). The step rate is `--hz`. The
victim stands still. The activator is the victim unless `--act-pos` is given, and `--sweep` tries
24 activator facings.

What it leaves out: other entities (players, other brush entities), the victim's own `pmove`
(an idle player), touch re-triggering, and `RecursiveHullCheck` (the push trace is a sampled
search to 2⁻³⁰ of the move, backed off by `DIST_EPSILON`). It is a model of these two code paths,
not the engine.

### Case 1: `dod_thunder2`, door entity #241 (model `*103`), a real wedge

Map md5 `68d6c078e63d7c32e75cc3de1ca5793f`. The door has its hinge at `(-152 -920 -66)`, a 56-unit
leaf, 90° travel, speed 100, wait 4, `TeamDoors 0`.

What the logs give (2026-11-12, the last seconds of a match, ending with a changelevel):
- In the last 30 s before the changelevel, one player was within 140 units of the hinge:
  `position_sample` at `(-114 -956 -76)`, then `(-111 -964 -76)` twice, then `(-97 -944 -76)`.
- The `push:` interval that straddles the changelevel carries 2,306 blocked steps on one pusher,
  one victim. Its `push_detail:` prints edict 172 as `(-/-)`, because the names are read after the
  map changed, so the door is identified by position: #241 is the only door near that player.

```
python doorsim.py dod_thunder2.bsp 241 -111 -964 -76 --start-yaw -90 --sweep
```

With the door closing from its south-open position, the leaf carries the player from
`(-111 -964)` to `(-96.87 -944.03)` and wedges him against the wall east of the doorway. The logs
recorded him at `(-97 -944)`. Then:

| Variant | Facings that hold (of 24) | Hold in 10 s |
|---|---|---|
| fleet: engine approximation, DoD DLL, 500 Hz | 12 | 9.43 s |
| `--hz 100` / `--hz 1000` | 12 / 12 | 9.42 s / 9.43 s |
| `--exact` (option C) | 12 | 9.65 s |
| `--sdk` (stock door code) | 12 (the other half) | 9.43 s |
| `--keep-side` | **0** | 2 steps per cycle |

Trace of one holding facing (`--yaw 180 --trace`): the leaf meets the player at about 0.57 s,
stops at −43° and stays there. The state alternates 2/3 every 0.25 s, and blocked steps rise by about 125 per 0.25 s.

### Case 2: `dod_armory_b7`, door entity #179 (model `*48`, the map's only door), a phantom contact

Map md5 `2b7510a7dc9c5cadbb54a63900bf9781`. The door has its hinge at `(904 808 -562)`. The leaf runs
48 units south and has a 5-unit stub past the hinge.

What the logs give (2026-11-11, a public session): the `push_detail:` victim slot is a player who
scored a kill from `(928 799 -590)` (crouched: origin 18 units above the floor) during the hold
interval, facing yaw −63.7. That position is about 3 units inside the crouched world hull, so the
model starts him at `(928 794 -590)`.

```
python doorsim.py dod_armory_b7.bsp 179 928 794 -590 --hull 3 --start-yaw -90 --yaw -63.7
```

| Variant | Result |
|---|---|
| fleet (approximation, DoD DLL, 500 Hz) | holds 9.67 s of 10 at −67°, victim ends at `(930.28 798.97)` |
| `--hz 100` / `--hz 1000` | holds 9.67 s / 9.67 s |
| `--exact` (option C) | **no contact at all**: the true leaf never reaches him |
| `--keep-side` | 3 blocked steps, door returns to −90° |
| `--sweep`, fleet | 12 of 24 facings hold, the logged −63.7 among them |

The door state at onset (closing from the west-open position) is inferred, not logged. It is the
only state in which his position is clear of the leaf and a block follows.

### Not reconstructed

- The baseline-window (11-03 to 11-10) holds: logs before stats_logging's position samples were
  sparse for public play.
- `dod_harrington` and `dod_lennon5_b1`: no position near the door in the window.

### Manual reproduction on a real server (two players, or one)

1. Load `dod_thunder2` with `ktp_profile_frame 1` (any `ktp_pusher_step_hz`).
2. Find the door hinged at `(-152 -920)`: its leaf closes along the wall line toward the east.
   Open it from the north side by walking into the doorway facing south. It swings south.
3. Step through and stand on the south side, about 40 units east of the hinge and 45 units south of
   the wall, inside the arc the leaf will sweep when it closes. Wait out the 4 s.
4. As the leaf closes on you, face west and keep still. It carries you into the corner between the
   leaf and the wall east of the doorway, then stops at about −43° and stays. The next `push:` line
   reads `blocked_ms` ≈ 9,000–10,000.
5. Turn to face east. The door reopens within 0.5 s and the next interval reads a few ms.

Control: repeat facing east from the start. The door bounces back once per cycle, a few ms per
interval. With one player, the opener is also the victim. With two, only the opener's facing
matters.

### What an automated engine-level reproduction still needs

The Lane B harness (KTPInfrastructure) already drives real `dod.so` bots through AMXX drive
plugins. A door scenario needs three things:

- a drive plugin that sets one bot's origin to the wedge position and another's yaw (ReAPI
  `set_entvar`), then has the second bot touch the door;
- `dod_thunder2` (or `dod_armory_b7`) in the Lane B image;
- a dispatched run, which is the operator's call because the tier-2 runner is on the production data
  server.

The assertion would be on `push:` `blocked_ms`. It cannot be run from a workstation under current
rules (no local Docker suites).

## Design options

### R: reversal guard (recommended first)

**What changes.** In `SV_PushRotate`'s block path, record the door's `avelocity` before calling
`pfnBlocked`. Afterwards, if the door is a `func_door_rotating` and its new `avelocity` still turns
the same way as the blocked step, the game re-targeted through the blocker. Re-target the move to
the mirror side, `m_vecAngle1 - (newFinal - m_vecAngle1)`, which is `-newFinal` for every door in
the rotation. Only a `DoorGoUp` re-target can be mirrored. A `DoorGoDown` target is the closed
angle itself, and a `DoorGoDown` into the blocker only follows an earlier bad `DoorGoUp`, which
the guard has already corrected. That means setting `avelocity`, `nextthink = ltime + travel`, and
`m_vecFinalAngle`. The last one is the hard part: it is game private data (`this + 0x1e8` in this
`dod_i386.so`). If it is left alone, `AngularMoveDone` snaps the door back to the wrong side, a
teleport of up to 180°.

**Safety.** Gate it behind a cvar defaulting to 0. Before writing, check that the private field
holds exactly `angles + avelocity * (nextthink - ltime)`, which the engine can compute from
`entvars`. If it does not, do nothing and count a `push:` field such as `guard_skip=`. That makes
the guard inert on any other game DLL or build.

**Effect in the model.** 0 of 24 facings hold in both cases, at 100, 500 and 1000 Hz, with either
collision model.

**Gameplay.** A door blocked while closing reopens toward the side it came from, every time,
instead of on a coin flip set by the opener's view. That is what players expect, and what DoD's
own sign flip seems meant to do. Swing speed, wait, damage and solidity are unchanged. It is the
same on every instance once fielded.

**Risks.**
- A write into game private data. The self-check above bounds it.
- The guard runs only when the game actually re-targeted. Calls swallowed by the 0.25 s limit
  change nothing, so a blocked door still presses for up to 0.25 s per contact. That is the
  existing behaviour of a good reversal.
- `pfnBlocked` side effects (`dmg`, sibling doors with the same `targetname`) are untouched. Every
  rotation door has `dmg 0` and no `targetname`.

**Demos and HLTV.** Unaffected: door angles are networked as entity state like any other motion.

**Other mods.** Keyed on classname and the field self-check. Off by default.

### R′: patch the game DLL instead

Skip the activator facing test in `DoorGoUp` when it is entered from `TS_GOING_DOWN`, so DoD's own
flip produces keep-side. It is a few bytes and needs no engine change. But it modifies a closed
Valve binary and gives the fleet a new game-DLL identity to pin and roll back. It would also make a
door with `spawnflags & 2` (reverse) pick the wrong side on every blocked close, because DoD's flip
assumes the default direction. None are in the rotation today. Listed so the ruling can choose it knowingly.

### C: rotated-brush collision (the carded option)

**What changes.** For a rotated `SOLID_BSP` entity, replace the rotate-the-endpoints test with a
true test of the world-aligned player box against the rotated brush. Every rotation door is a
single box slab, so an analytic box-vs-oriented-box sweep (separating axes) covers them. Other
models fall back to today's path. The test lives in two places that must change together:
`SV_SingleClipMoveToEntity` (`world.cpp`) and the server `pmove` tests `_PM_TestPlayerPosition`
and `_PM_PlayerTrace` (`pmovetst.cpp`). If only the pusher test changes, the door stops "seeing"
players that `pmove` still counts as inside it. That creates new stuck-in-door cases, the thing
being fixed.

**Effect in the model.** Removes the armory hold (no contact at all) and leaves the thunder2 hold
untouched (12 of 24 facings).

**Risks.**
- **Client prediction.** The client's `pmove` runs in the stock client engine and keeps the
  approximation, so a player touching a door mid-swing is predicted against one shape and corrected
  to another. The error is a few units, only during a swing (0° and ±90° are exact for a square
  footprint), and shows as a small correction rather than a stick.
- **Blast radius.** Every rotated `SOLID_BSP` brush on any map, not just doors. The rotation's
  doors rest at 0° and ±90°, where nothing changes.
- **Implementation size.** Medium. The collision code has unit tests but no rotated-brush cases.
- **Demos and HLTV.** Unaffected on playback. A demo stores entity states, not collisions.

### Not recommended

- **Another step rate.** No effect on either reproduced hold at 100, 500 or 1000 Hz.
- **An unstick nudge.** Refuted earlier: it can push a player through geometry, and it acts on the
  wrong population.

## How a test would prove it before any wave

1. **Unit (cppunitlite, `rehlds/unittests`).**
   - For R: the guard's decision on synthetic `(angles, avelocity before/after, nextthink, ltime,
     private field)` tuples, including the self-check refusing a mismatched field.
   - For C: rotated-box cases, including the armory hinge-stub geometry (contact under the
     approximation, none under the true test) and 0°/90° equivalence.
2. **Model regression.** The two `doorsim.py` commands above, with their expected outputs in this
   document.
3. **Lane B scenario** (once built, see above).
   - Guard off: a holding facing reads `blocked_ms` ≥ 9,000 per interval and a releasing facing ≤ 10.
   - Guard on: both read ≤ 10.
   - Run it on thunder2 #241 for R and on armory #179 for C.
4. **Fleet read after a canary.** The long-hold metric already in use: intervals with ≥ 1 s of
   blocking on the door maps per player-hour. The 11-18 week read 4 in 497 player-hours. Report it
   beside `blocked_ms` per player-hour. The short-hold distribution (one ~2 ms step per clean
   reversal) should not move.

HLTV demos cannot replay collisions, but they can confirm a canary from the outside: in recorded
matches, no door angle should stall mid-swing for more than 0.5 s.

## Corrections to earlier records

- "`CBaseDoor::Blocked` reverses the door on every call": true of the SDK, not of DoD. DoD acts at
  most once per 0.25 s of server time. The per-frame block/reverse oscillation inferred from
  `episodes ≈ blocked` was therefore not the game reversing every frame. What did produce that
  signature is still open.
- "The long holds are the rotated-brush approximation": partly. It creates phantom contacts (case
  2). The ingredient common to both reproduced holds is the reversal side.

## Decisions requested

1. Build the reversal guard R behind a default-0 cvar, with unit tests and the field self-check
   (a), or patch the game DLL R′ (b)? Recommended: a.
2. Build option C after R, measured separately (a), or hold it until R's canary reads (b)?
   Recommended: b. R removes both reproduced holds, and C carries the client-prediction risk.
3. Add the Lane B door scenario (a drive plugin plus a door map in the image) as the pre-wave proof
   for either change (a), or rely on unit tests plus the model and a canary (b)? Recommended: a.
