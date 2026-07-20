# KTP-ReHLDS

**Version 3.22.0.929** | Custom ReHLDS fork for KTP competitive Day of Defeat infrastructure

A specialized fork of [ReHLDS](https://github.com/rehlds/rehlds) that enables advanced competitive match features through engine-level pause control, real-time HUD updates during pause, and selective subsystem operation.

---

## Purpose

Standard GoldSrc/ReHLDS pause (`pausable 1` + `pause`) completely freezes the server - game simulation, chat, HUD updates, and client commands all stop. This creates critical UX issues in competitive matches: when a player disconnects and auto-pause triggers, nobody can communicate or coordinate unpause.

**KTP-ReHLDS solves this** by allowing selective subsystem operation during pause:
- Game simulation stays frozen (physics, time, entities)
- Communication continues (network I/O, HUD messages, commands)
- Real-time HUD countdown timers via `SV_UpdatePausedHUD` hook

---

## Architecture Position

KTP-ReHLDS is the **engine foundation** (Layer 1) of the KTP competitive stack:

```
Layer 6: Application Plugins (KTPMatchHandler, KTPAdminAudit, etc.)
Layer 5: Game Stats Module (DODX)
Layer 4: HTTP Module (KTP AMXX Curl)
Layer 3: Engine Bridge (KTP-ReAPI)
Layer 2: Scripting Platform (KTPAMXX)
Layer 1: Game Engine (KTP-ReHLDS) <-- YOU ARE HERE
```

---

## Key Features

### Selective Pause System

**Game Stays Frozen:**
- Physics simulation (`SV_Physics()` skipped)
- Game time frozen (`g_psv.time` stops advancing)
- Entity thinking disabled, player movement blocked
- `host_frametime` = 0 (complete time freeze)

**Communication Continues:**
- Network I/O processes normally
- Server messages work (`rcon say`, join/leave events)
- HUD messages sent (`MSG_WriteByte(svc_*)`)
- Client message buffers flushed (`SV_SendClientMessages()`)
- Commands processed (`/cancel`, `/pause`, etc.)
- Player chat: first message works, subsequent blocked by DoD DLL flood protection

### Engine-Level Pause Control

Works with `pausable 0` - standard engine pause disabled, only KTP pause system works via ReAPI `rh_set_server_pause()`. Prevents abuse of engine pause command while giving full control to AMX plugins.

### Silent Pause Mode (v3.22.0+)

**Cvar: `ktp_silent_pause`**
- `0` (default): Normal - clients see "PAUSED" overlay
- `1`: Silent - no overlay, custom HUD still works via `RH_SV_UpdatePausedHUD`
- Physics remain frozen server-side

### Blocked Kick/Ban Commands (v3.19.0+)

`kick`, `banid`, `removeid`, `addip`, `removeip` blocked at engine level. Forces all player removals through KTPAdminAudit plugin for full audit trail with admin name, SteamID, and timestamp.

### Real-Time HUD Updates During Pause

**Engine Hook: `SV_UpdatePausedHUD()`** - called every frame during pause (~60-1000 Hz). Allows plugins to update HUD with live countdown timers, unpause countdowns, and warnings.

### Frame Profiling (v3.22.0.904+)

Low-overhead built-in profiling for identifying performance bottlenecks.

**CVars:**
- `ktp_profile_frame 0/1` - Enable/disable
- `ktp_profile_interval 10` - Seconds between summary logs

**Metrics:** `read` (SV_ReadPackets), `phys` (SV_Physics), `send` (SV_SendClientMessages), `edicts_max` (peak entity count). Reports average and peak times per interval. See `CLAUDE.md` § Frame Profiling for the full metric set (`misc1`, `post`, `steam`, `full`, `gap`, the `io:` line) and the `[KTP_SPIKE*]` alert lines.

### Host_FilterTime FPS Fix (v3.22.0.904+)

Fixed artificial FPS cap: original `1.0f / (fps + 1.0f)` limited servers to sys_ticrate - 1. Changed to `1.0 / fps` with `double` precision for true 1000 fps at sys_ticrate 1000.

### Hostname Broadcast (v3.21.0+)

**Cvar: `ktp_hostname_broadcast`**
- `0` (default): Update serverinfo only (safe, no disruption)
- `1`: Broadcast to clients (causes respawn - NOT for mid-match use)
- Auto-resets to 0 after broadcast (one-shot)

### Async Log-File Writer (v3.22.0.927+)

The synchronous log write in `Log_Printf` blocked the game thread for up to 167ms
on consumer-SSD journal stalls. A dedicated writer thread now owns the log
`FILE*`; the game thread only enqueues into a 2048-slot ring and never blocks (a
full queue drops the line and counts it).

**Cvar: `ktp_log_async`**
- `1` (default): writer thread owns the file
- `0`: exact legacy synchronous path — the rollback switch, no binary swap needed

The mode latches per log session at `Log_Open`, so a change takes effect at the
next map change. Writer-side timings land on the `[KTP_PROFILE] io:` line; see
`CLAUDE.md` § Async Log-File Writer for the field reference.

---

## KTP CVars

| Cvar | Default | Purpose |
|------|---------|---------|
| `ktp_silent_pause` | `0` | `1` suppresses the client PAUSED overlay; physics stay frozen |
| `ktp_hostname_broadcast` | `0` | `1` broadcasts a hostname change to clients (causes respawn); one-shot |
| `ktp_profile_frame` | `0` | Enable frame profiling |
| `ktp_profile_interval` | `10` | Seconds between profiling summary logs |
| `ktp_profile_spike_threshold` | `5.0` | Log a `[KTP_SPIKE]` alert when a frame exceeds this many ms (`0` = off) |
| `ktp_profile_steam_detail` | `0` | Granular `Steam_RunFrame()` sub-timing |
| `ktp_log_async` | `1` | Async log-file writer; `0` restores the synchronous path (rollback switch) |
| `ktp_extension_loaded` | `0` | Sentinel — counts extensions loaded from `extensions.ini`. Assert `>= 1` after a restart |
| `ktp_userinfo_hook` | `1` | `SV_ClientUserInfoChanged` hookchain dispatch; `0` disables it live (rollback switch) |

Full detail in `CLAUDE.md` and [CHANGELOG.md](CHANGELOG.md).

---

## Extension Mode Hooks

Hookchains added for KTPAMXX/DODX extension mode compatibility (no Metamod):

### Core KTP Hooks

| Hook | Purpose |
|------|---------|
| `SV_UpdatePausedHUD` | Real-time HUD updates during pause |
| `SV_Rcon` | RCON command audit logging (command, IP, validity). **.928:** fires on *every* attempt with the real validity flag; was success-only with validity hardcoded true. The password is never included in the audited string |
| `Host_Changelevel_f` | Console changelevel command interception |

### KTPAMXX Extension Mode Hooks

| Hook | Purpose | Replaces |
|------|---------|----------|
| `SV_ClientCommand` | Chat commands, menus | `register_clcmd`, `client_command` |
| `SV_InactivateClients` | Map change cleanup | `plugin_end`, `client_disconnect` |
| `SV_ClientUserInfoChanged` | Client info changes | `client_infochanged` |
| `PF_RegUserMsg_I` | Message ID capture | HUD drawing |
| `PF_changelevel_I` | Game DLL changelevel | `server_changelevel` |
| `PF_setmodel_I` | Entity model tracking | Model monitoring |
| `AlertMessage` | Engine log messages | `register_logevent` |
| `PF_TraceLine` | TraceLine interception | DODX `TraceLine_Post` |
| `PF_SetClientKeyValue` | Client key/value changes | DODX `SetClientKeyValue` |
| `SV_PlayerRunPreThink` | Player PreThink loop | DODX stats tracking |

### Extension Shutdown Callback (v3.22.0.928+)

Extensions may optionally export `KTP_ExtensionShutdown`. The engine `dlsym`s it
on each loaded extension and calls it in `ReleaseEntityDlls` before the `dlclose`
loop, giving the extension a chance to detach its own modules — extension mode
never runs Metamod's `Meta_Detach`, so without this there is no teardown hook at
all. Requires KTPAMXX 2.7.21+ to do anything; the engine is a no-op against an
extension that doesn't export it. A dll listed twice in `extensions.ini` gets the
callback twice.

`KTP_ClearAllHooks()` empties every hookchain registry before the `dlclose`
regardless of what the extension does — the backstop that holds even if an
extension misbehaves.

### Message Registration Fix

`RegUserMsg_internal` now searches both `sv_gpUserMsgs` (sent) and `sv_gpNewUserMsgs` (pending), preventing duplicate message IDs (130+) when KTPAMXX looks up existing messages.

---

## Build Instructions

### Linux (or WSL)

From the repo root:

```bash
bash build.sh -j=$(nproc)
```

Output: `build/rehlds/engine_i486.so` and `build/rehlds/dedicated/hlds_linux`

`build.sh` is the build — it is all a clone needs. KTP maintainers additionally
have a `build_linux.sh` wrapper in the repo root, but it is **gitignored**: it
hardcodes a box-specific path to auto-stage artifacts into a local
`KTP DoD Server/serverfiles/` test tree. Don't look for it in a clone.

**Requirements:**
- cmake >= 3.1
- GCC >= 4.9.2 with 32-bit multilib (`sudo apt-get install gcc-multilib g++-multilib`)

**Compiler options:**
```
./build.sh --compiler=[icc|gcc|clang] --jobs=[N]
```

### Windows

1. Open `msvc/ReHLDS.sln` in Visual Studio 2022
2. Select `Release` / `Win32`
3. Build -> Rebuild Solution

Output: `msvc/Release/swds.dll`

---

## Installation

### KTP Servers

The engine binary is deployed as the server executable:

```
serverfiles/engine_i486.so    (Linux)
serverfiles/hlds_linux         (Linux)
```

**Server configuration:**
```
// Disable engine pause - KTP-ReAPI/KTPMatchHandler handle pausing
pausable 0
```

**Extension loading** via `<gamedir>/addons/extensions.ini` (for DoD:
`dod/addons/extensions.ini`):
```
addons/ktpamx/dlls/ktpamx_i386.so
```
Each line resolves relative to the game directory (`sys_dll.cpp:1123`). If the
file is missing the loader returns silently and the server runs as vanilla
HLDS — verify with the `ktp_extension_loaded` cvar rather than assuming.

### Verify Installation

Check server console on startup:
```
========================================
  KTP ReHLDS - Custom Pause Build
  HUD enabled during pause
  Version: <APP_VERSION>
========================================
```

The banner prints the generated `APP_VERSION`, whose commit-count component runs
ahead of the CHANGELOG title — it is not a deploy check. **Verify a deploy by the
md5 of `engine_i486.so`, not by the banner.**

Check modules loaded: `amxx modules` in server console.

---

## Version Information

- **Current Version**: 3.22.0.929 (2026-07)
- **Based on**: ReHLDS 3.14.0.857 (upstream)
- **Platform**: Visual Studio 2022 (v143) / GCC 4.9.2+ / Clang 6.0+
- **Compatible with**: KTP-ReAPI 5.29.0.362-ktp+, KTPAMXX 2.7.21+ (`KTP_ExtensionShutdown` requires 2.7.21+; on older KTPAMXX the .928/.929 shutdown-safety work is inert)

See [CHANGELOG.md](CHANGELOG.md) for full version history.

---

## Related Projects

**KTP Stack:**
- [KTPAMXX](https://github.com/afraznein/KTPAMXX) - AMX Mod X fork (loaded as extension)
- [KTP-ReAPI](https://github.com/afraznein/KTPReAPI) - ReAPI fork (engine bridge)
- [KTPMatchHandler](https://github.com/afraznein/KTPMatchHandler) - Match management (primary consumer)
- [KTPAdminAudit](https://github.com/afraznein/KTPAdminAudit) - RCON audit logging

**Upstream:**
- [ReHLDS](https://github.com/rehlds/rehlds) - Original project. Its original README is preserved in-tree as [README-UPSTREAM.md](README-UPSTREAM.md)
- [AMX Mod X](https://www.amxmodx.org/) - Plugin platform

---

## License

MIT License - Same as upstream ReHLDS. See [LICENSE](LICENSE) for full text.
