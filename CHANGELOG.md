# [ReHLDS](https://github.com/ReHLDS/ReHLDS) Changelog

**ReHLDS** is a result of reverse engineering of original `HLDS` (build `6152`/`6153`) using `DWARF` debug info embedded into linux version of `HLDS`, `engine_i486.so`.

Along with reverse engineering, a lot of defects and (potential) bugs were found and fixed.

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

## [KTP-ReHLDS `3.15.0.888-dev+m`] - 2025-11-20

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
