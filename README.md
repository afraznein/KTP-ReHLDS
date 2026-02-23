# KTP-ReHLDS

**Version 3.22.0.904+** | Custom ReHLDS fork for KTP competitive Day of Defeat infrastructure

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

**Metrics:** `read` (SV_ReadPackets), `phys` (SV_Physics), `send` (SV_SendClientMessages), `edicts_max` (peak entity count). Reports average and peak times per interval.

### Host_FilterTime FPS Fix (v3.22.0.904+)

Fixed artificial FPS cap: original `1.0f / (fps + 1.0f)` limited servers to sys_ticrate - 1. Changed to `1.0 / fps` with `double` precision for true 1000 fps at sys_ticrate 1000.

### Hostname Broadcast (v3.21.0+)

**Cvar: `ktp_hostname_broadcast`**
- `0` (default): Update serverinfo only (safe, no disruption)
- `1`: Broadcast to clients (causes respawn - NOT for mid-match use)
- Auto-resets to 0 after broadcast (one-shot)

---

## Extension Mode Hooks

Hookchains added for KTPAMXX/DODX extension mode compatibility (no Metamod):

### Core KTP Hooks

| Hook | Purpose |
|------|---------|
| `SV_UpdatePausedHUD` | Real-time HUD updates during pause |
| `SV_Rcon` | RCON command audit logging (command, IP, validity) |
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

### Message Registration Fix

`RegUserMsg_internal` now searches both `sv_gpUserMsgs` (sent) and `sv_gpNewUserMsgs` (pending), preventing duplicate message IDs (130+) when KTPAMXX looks up existing messages.

---

## Build Instructions

### Linux (WSL)

```bash
wsl bash -c "cd '/mnt/n/Nein_/KTP Git Projects/KTPReHLDS' && bash build_linux.sh"
```

Output: `rehlds/build/rehlds/engine_i486.so` and `rehlds/build/dedicated/hlds_linux`

The build script auto-stages to `KTP DoD Server/serverfiles/`.

**Requirements:**
- cmake >= 3.1
- GCC >= 4.9.2 with 32-bit multilib (`sudo apt-get install gcc-multilib g++-multilib`)

**Compiler options:**
```
cd rehlds && ./build.sh --compiler=[icc|gcc|clang] --jobs=[N]
```

### Windows

1. Open `rehlds/msvc/rehlds.sln` in Visual Studio 2022
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

**Extension loading** via `rehlds/extensions.ini`:
```
ktpamx/dlls/ktpamx_i386.so
```

### Verify Installation

Check server console on startup:
```
========================================
  KTP ReHLDS - Custom Pause Build
  HUD enabled during pause
  Version: 3.22.0.904-dev+m (Feb  6 2026)
========================================
```

Check modules loaded: `amxx modules` in server console.

---

## Version Information

- **Current Version**: 3.22.0.904-dev+m (2026-02)
- **Based on**: ReHLDS 3.14.0.857 (upstream)
- **Platform**: Visual Studio 2022 (v143) / GCC 4.9.2+ / Clang 6.0+
- **Compatible with**: KTP-ReAPI 5.29.0.362-ktp+, KTPAMXX 2.6.9+

See [CHANGELOG.md](CHANGELOG.md) for full version history.

---

## Related Projects

**KTP Stack:**
- [KTPAMXX](https://github.com/afraznein/KTPAMXX) - AMX Mod X fork (loaded as extension)
- [KTP-ReAPI](https://github.com/afraznein/KTPReAPI) - ReAPI fork (engine bridge)
- [KTPMatchHandler](https://github.com/afraznein/KTPMatchHandler) - Match management (primary consumer)
- [KTPAdminAudit](https://github.com/afraznein/KTPAdminAudit) - RCON audit logging

**Upstream:**
- [ReHLDS](https://github.com/rehlds/rehlds) - Original project
- [AMX Mod X](https://www.amxmodx.org/) - Plugin platform

---

## License

MIT License - Same as upstream ReHLDS. See [LICENSE](LICENSE) for full text.
