# KTP-ReHLDS

**Custom ReHLDS fork with selective pause system and real-time HUD updates**

A specialized fork of [ReHLDS](https://github.com/rehlds/rehlds) that enables advanced competitive match features through engine-level pause control, HUD updates during pause, and selective subsystem operation.

---

## 🎯 Purpose

Standard GoldSrc/ReHLDS pause (`pausable 1` + `pause`) completely freezes the server, including:
- ❌ **Game simulation** (desired - freeze gameplay)
- ❌ **Chat system** (problematic - can't communicate)
- ❌ **HUD updates** (problematic - no countdown timers)
- ❌ **Client commands** (problematic - can't use `/confirmunpause`)

**This creates critical UX issues in competitive matches:**

1. Player disconnects → auto-pause triggered
2. Player reconnects during pause
3. **Nobody knows they're back** - chat frozen
4. Teams can't coordinate unpause - commands frozen
5. **Result**: Confusion, delays, poor player experience

**KTP-ReHLDS solves this** by allowing selective subsystem operation during pause.

---

## 🏗️ Architecture Position

KTP-ReHLDS is the **engine foundation** of the KTP competitive stack:

```
┌─────────────────────────────────────────────────┐
│  Layer 4: KTPMatchHandler (AMX Plugin)          │
│  Match logic, pause workflow, Discord           │
└─────────────────────────────────────────────────┘
                     ↓ Uses hooks from
┌─────────────────────────────────────────────────┐
│  Layer 3: KTPCvarChecker (AMX Plugin)           │
│  Anti-cheat, cvar enforcement                   │
└─────────────────────────────────────────────────┘
                     ↓ Calls ReAPI hooks
┌─────────────────────────────────────────────────┐
│  Layer 2: KTP-ReAPI (AMX Module)                │
│  Exposes RH_SV_UpdatePausedHUD hook             │
└─────────────────────────────────────────────────┘
                     ↓ Bridges to
┌─────────────────────────────────────────────────┐
│  Layer 1: KTP-ReHLDS (Engine) ← YOU ARE HERE    │
│  Provides: SV_UpdatePausedHUD() function        │
│  Features: Pause with pausable 0, HUD updates   │
└─────────────────────────────────────────────────┘
```

---

## ✨ Key Features

### 🎮 Selective Pause System

**Game Stays Frozen:**
- ✅ Physics simulation (`SV_Physics()` skipped)
- ✅ Game time frozen (`g_psv.time` stops advancing)
- ✅ Entity thinking disabled
- ✅ Player movement blocked
- ✅ `host_frametime` = 0 (complete time freeze)

**Communication Continues:**
- ✅ Network I/O processes
- ✅ Server messages work (`rcon say`, join/leave events)
- ✅ HUD messages sent (`MSG_WriteByte(svc_*)`)
- ✅ Client message buffers flushed (`SV_SendClientMessages()`)
- ✅ Commands processed (`/cancel`, `/pause`, etc.)
- ⚠️ Player chat (first message works, subsequent blocked by DoD DLL - use RCON as workaround)

### 🔒 Blocked Kick/Ban Commands (v3.19.0+)

**Audit-enforced player removal:**
- `kick`, `banid`, `removeid`, `addip`, `removeip` commands blocked at engine level
- Prevents untraceable RCON/console kicks and bans
- Directs admins to use `.kick` and `.ban` in-game commands
- Works with KTPAdminAudit for full audit logging
- All blocked attempts logged for security review

### 🔧 Engine-Level Pause Control

**Works with `pausable 0`:**
- Standard engine pause (`pausable 1`) disabled
- Only KTP pause system works (via ReAPI `rh_set_server_pause()`)
- Prevents abuse of engine pause command
- Full control from AMX plugins

### 📊 Real-Time HUD Updates During Pause

**New Engine Hook: `SV_UpdatePausedHUD()`**
- Called **every frame** during pause (~60-100 Hz)
- Allows plugins to update HUD with live timers
- Displays MM:SS countdown for pause duration
- Shows unpause countdowns (5...4...3...2...1)
- Updates warnings (30s, 10s remaining)

### 🔌 ReAPI Integration

**Pause HUD Hook:**
```cpp
// rehlds/public/rehlds/rehlds_api.h
class IRehldsHook_SV_UpdatePausedHUD {
public:
    virtual ~IRehldsHook_SV_UpdatePausedHUD() {}
    virtual void SV_UpdatePausedHUD() = 0;
};
```

### 🔗 KTPAMXX Extension Mode Support

**New hooks for AMX Mod X extension mode (v3.16.0+, expanded in v3.17.0):**

| Hook | Purpose | Used By |
|------|---------|---------|
| `SV_ClientCommand` | Chat commands, menus | `register_clcmd`, `client_command` |
| `SV_InactivateClients` | Map change cleanup | `plugin_end`, `client_disconnect` |
| `SV_ClientUserInfoChanged` | Client info changes | `client_infochanged` |
| `PF_RegUserMsg_I` | Message ID capture | HUD drawing, `client_print` |
| `PF_changelevel_I` | Level change | `server_changelevel` |
| `PF_setmodel_I` | Entity model tracking | Model monitoring |
| `AlertMessage` | Engine log messages | `register_logevent` |
| `PF_TraceLine` | TraceLine interception | DODX `TraceLine_Post` |
| `PF_SetClientKeyValue` | Client key/value changes | DODX `SetClientKeyValue` |
| `SV_PlayerRunPreThink` | Player PreThink loop | DODX stats tracking, shot detection |

**Message Registration Fix:**
- `RegUserMsg_internal` now searches BOTH `sv_gpUserMsgs` AND `sv_gpNewUserMsgs`
- Prevents duplicate message IDs (130+) when KTPAMXX looks up existing messages

**Called from Engine:**
```cpp
// rehlds/engine/sv_main.cpp
void SV_Frame() {
    if (g_RehldsHookchains.m_SV_UpdatePausedHUD) {
        g_RehldsHookchains.m_SV_UpdatePausedHUD->callChain();
    }
}
```

**Consumed by KTP-ReAPI:**
- KTP-ReAPI registers handler for this hook
- Exposes to AMX plugins as `RH_SV_UpdatePausedHUD`
- KTPMatchHandler uses it for real-time HUD

---

## 🔬 Technical Implementation

### Engine Modifications

#### 1. **Pause Hook System** (`rehlds/public/rehlds/rehlds_api.h`)

Added new hook interface and hookchain:
```cpp
// Hook interface
class IRehldsHook_SV_UpdatePausedHUD {
public:
    virtual ~IRehldsHook_SV_UpdatePausedHUD() {}
    virtual void SV_UpdatePausedHUD() = 0;
};

// Hook chain registry
class IRehldsHookchains {
    // ... existing hooks ...
    IRehldsHookRegistry_SV_UpdatePausedHUD* m_SV_UpdatePausedHUD;
};
```

#### 2. **Frame Processing** (`rehlds/rehlds/engine/sv_main.cpp`)

Modified `SV_Frame()` to call pause hook:
```cpp
void SV_Frame() {
    // ... existing frame processing ...

    // Call pause HUD update hook (if registered)
    if (g_RehldsHookchains.m_SV_UpdatePausedHUD) {
        g_RehldsHookchains.m_SV_UpdatePausedHUD->callChain();
    }

    // ... continue frame processing ...
}
```

**Key Changes:**
- Hook called every frame, regardless of pause state
- Plugins check pause state internally
- Zero overhead when no hooks registered

#### 3. **Message Buffer Handling** (`rehlds/rehlds/engine/sv_user.cpp`)

Ensured `SV_SendClientMessages()` processes during pause:
```cpp
void SV_SendClientMessages() {
    // Process message buffers even during pause
    // This allows HUD messages to reach clients
    for (int i = 0; i < svs.maxclients; i++) {
        client_t* client = &svs.clients[i];
        if (client->active || client->spawned) {
            SV_SendClientDatagram(client);
        }
    }
}
```

**Why This Works:**
- Message buffers (`MSG_WriteByte()`) accumulate during pause
- `SV_SendClientMessages()` flushes buffers to network
- Clients receive and render HUD updates in real-time

#### 4. **Chat During Pause**

**Current Status:**
- ✅ `rcon say` works (bypasses client command processing)
- ✅ Server events work (join/leave use direct buffer writes)
- ✅ Commands processed (`/cancel`, `/pause`, etc.)
- ✅ First chat message per pause works
- ⚠️ Subsequent messages blocked by DoD game DLL flood protection

**Technical Implementation:**
- Frame-wide temporary unpause system (`g_ktp_temporary_unpause` flag)
- `g_psv.paused` temporarily cleared for entire frame, restored after `SV_SendClientMessages()`
- ReHLDS string command rate limiter bypassed during temporary unpause
- Game time (`g_psv.time`) remains frozen - match timer unaffected
- Physics simulation still skipped via `shouldSimulate` check

**Known Limitation:**
- DoD game DLL has built-in chat flood protection checking frozen game time
- Only first message per pause works for client "say" commands
- Workaround: Use RCON messages for subsequent communication
- Full fix requires DoD game DLL source code modifications

### Modified Files Summary

| File | Purpose | Changes |
|------|---------|---------|
| `rehlds/public/rehlds/rehlds_api.h` | API definitions | Added `IRehldsHook_SV_UpdatePausedHUD` interface |
| `rehlds/rehlds/engine/sv_main.cpp` | Frame processing | Added hook call in `SV_Frame()` |
| `rehlds/rehlds/engine/sv_user.cpp` | Client messages | Ensured message flushing during pause |
| `rehlds/rehlds/public/rehlds/hookchains.h` | Hook registry | Registered new hook in chainregistry |

---

## 🆚 Comparison: Standard ReHLDS vs KTP-ReHLDS

| Feature | Standard ReHLDS | KTP-ReHLDS |
|---------|----------------|------------|
| **Pause Method** | `pausable 1` + `pause` | ReAPI `rh_set_server_pause()` |
| **Works with `pausable 0`** | ❌ No | ✅ Yes |
| **HUD Updates During Pause** | ❌ Frozen | ✅ Real-time (60-100 Hz) |
| **Server Messages During Pause** | ❌ Frozen | ✅ Works (`rcon say`, events) |
| **Player Chat During Pause** | ❌ Frozen | ⚠️ WIP |
| **Kick/Ban Command Blocking** | ❌ Not available | ✅ Blocked for audit trail |
| **Custom Hook: SV_UpdatePausedHUD** | ❌ Not available | ✅ Available |
| **ReAPI Integration** | ✅ Standard hooks | ✅ Standard + KTP hooks |
| **Backward Compatibility** | ✅ N/A | ✅ Full compatibility |

---

## 💡 Real-World Impact

### Before KTP-ReHLDS:
```
Player disconnects during match

Plugin executes: pause (pausable 1)
Server freezes EVERYTHING
Players see: "⏸ PAUSED ⏸" [frozen forever]

Player reconnects...
Nobody knows! (chat frozen)
Admins must use RCON: rcon say "Player reconnected!"
Teams confused about unpause timing

Result: Delays, frustration, unprofessional experience
```

### After KTP-ReHLDS:
```
Player disconnects during match

Plugin executes: rh_set_server_pause(1)  (pausable 0)
KTP-ReHLDS pauses game, keeps HUD alive
Players see: "⏸ PAUSED ⏸"
             "04:37 remaining"
             "Waiting for player..."
             [Updates every frame!]

Player reconnects...
Server event: "Player has reconnected" (works during pause!)
HUD updates: "Player reconnected - type /confirmunpause"
Teams coordinate unpause smoothly

Result: Professional experience, clear communication
```

---

## 📋 Version Information

- **Current Version**: KTP-ReHLDS 3.19.0.895-dev+m (2025-12-21)
- **Previous Version**: 3.18.0.894-dev+m (2025-12-16) - PlayerPreThink hookchain
- **Based on**: ReHLDS 3.14.0.857 (upstream)
- **Platform Toolset**: Visual Studio 2022 (v143) for Windows
- **Compiler**: GCC 4.9.2+ or Clang 6.0+ for Linux
- **Compatible with**: ReAPI 5.26+, KTP-ReAPI 1.0+, **KTPAMXX 2.6.0+**

---

## 🚀 Quick Start

### For Server Administrators

**Prerequisites:**
- Existing Half-Life Dedicated Server (HLDS) installation
- AMX Mod X 1.9+ installed
- KTP-ReAPI module (for hook exposure)

**Installation Steps:**

1. **Backup existing engine:**
   ```bash
   # Linux
   cp <hlds>/engine_i486.so <hlds>/engine_i486.so.backup

   # Windows
   copy <hlds>\swds.dll <hlds>\swds.dll.backup
   ```

2. **Download KTP-ReHLDS:**
   ```bash
   # From releases
   # https://github.com/afraznein/KTP-ReHLDS/releases
   ```

3. **Install KTP-ReHLDS binary:**
   ```bash
   # Linux
   cp engine_i486.so <hlds>/

   # Windows
   copy swds.dll <hlds>\
   ```

4. **Configure server.cfg:**
   ```
   // CRITICAL: Disable engine pause
   pausable 0

   // Let ReAPI handle pause via rh_set_server_pause()
   // KTPMatchHandler will control pause
   ```

5. **Install KTP-ReAPI module:**
   ```bash
   # See: https://github.com/afraznein/KTP-ReAPI
   ```

6. **Install KTPMatchHandler plugin:**
   ```bash
   # See: https://github.com/afraznein/KTPMatchHandler
   ```

7. **Verify installation:**
   ```bash
   # Start server and check console
   # Should see: ReHLDS version 3.19.0.895-dev+m

   # In server console:
   meta version  # Check ReAPI loaded
   amxx list     # Check KTPMatchHandler loaded
   ```

---

## 🔧 Build Instructions

### For Developers

**Prerequisites:**

#### Windows
- Visual Studio 2022 (v143 toolset)
- CMake 3.10+ (optional, for Visual Studio project generation)

#### Linux
- CMake 3.10+
- GCC 4.9.2+ or Clang 6.0+
- 32-bit development libraries

**Building on Windows:**

1. Clone repository:
   ```bash
   git clone https://github.com/afraznein/KTP-ReHLDS.git
   cd KTP-ReHLDS/rehlds
   ```

2. Open Visual Studio solution:
   ```bash
   # Open msvc/rehlds.sln in Visual Studio 2022
   ```

3. Build:
   - Select configuration: `Release`
   - Select platform: `Win32`
   - Build → Rebuild Solution

4. Output:
   ```
   msvc/Release/swds.dll
   msvc/Release/swds.pdb  (debug symbols)
   ```

**Building on Linux:**

1. Clone repository:
   ```bash
   git clone https://github.com/afraznein/KTP-ReHLDS.git
   cd KTP-ReHLDS/rehlds
   ```

2. Install dependencies (Debian/Ubuntu):
   ```bash
   sudo dpkg --add-architecture i386
   sudo apt-get update
   sudo apt-get install -y gcc-multilib g++-multilib
   sudo apt-get install -y build-essential cmake
   sudo apt-get install -y libc6-dev libc6-dev-i386
   ```

3. Build using script:
   ```bash
   # GCC (default)
   ./build.sh --compiler=gcc

   # Clang (alternative)
   ./build.sh --compiler=clang

   # With debug symbols
   ./build.sh --compiler=gcc -DDEBUG=ON
   ```

4. Output:
   ```
   build/rehlds/engine_i486.so
   ```

**Development Build Options:**

```bash
# Build with multiple jobs (faster)
./build.sh --compiler=gcc --jobs=4

# Enable debug mode
./build.sh -DDEBUG=ON

# Static libstdc++ linking
./build.sh -DUSE_STATIC_LIBSTDC=ON
```

---

## 🎮 Integration with KTP Stack

### How KTPMatchHandler Uses KTP-ReHLDS

**Pause Workflow:**

1. **Player triggers pause:**
   ```pawn
   // In KTPMatchHandler.sma
   public CmdPause(id) {
       // Use ReAPI to pause (not engine command)
       rh_set_server_pause(1);  // KTP-ReHLDS handles this!

       // Start HUD updates
       g_bIsPaused = true;
       g_iPauseStartTime = get_systime();
   }
   ```

2. **KTP-ReHLDS processes pause:**
   ```cpp
   // In KTP-ReHLDS sv_main.cpp
   void SV_Frame() {
       if (sv.paused) {
           // Skip physics (game frozen)
           // But call pause hook for HUD updates
           g_RehldsHookchains.m_SV_UpdatePausedHUD->callChain();
       }
   }
   ```

3. **KTP-ReAPI forwards to plugin:**
   ```cpp
   // In KTP-ReAPI hook_callback.cpp
   void SV_UpdatePausedHUD() {
       // Call registered AMX plugin handlers
       callForward(g_forwards[FWD_SV_UpdatePausedHUD]);
   }
   ```

4. **KTPMatchHandler updates HUD:**
   ```pawn
   // In KTPMatchHandler.sma
   public OnPausedHUDUpdate() {
       new iRemaining = g_iPauseDuration - (get_systime() - g_iPauseStartTime);
       new iMinutes = iRemaining / 60;
       new iSeconds = iRemaining % 60;

       set_hudmessage(255, 255, 0, -1.0, 0.35, 0, 0.0, 0.1, 0.0, 0.0, -1);
       show_hudmessage(0, "⏸ PAUSED ⏸^n%02d:%02d", iMinutes, iSeconds);

       return HC_CONTINUE;
   }
   ```

**Result**: Players see real-time MM:SS countdown during pause!

---

## 🔗 Related KTP Projects

### **Complete KTP Competitive Infrastructure:**

**🔧 Engine Layer:**
- **[KTP-ReHLDS](https://github.com/afraznein/KTP-ReHLDS)** - This project
  - Selective pause system
  - Real-time HUD updates during pause
  - Chat during pause (WIP)
  - Provides `SV_UpdatePausedHUD()` engine hook

**🔌 Module Layer:**
- **[KTP-ReAPI](https://github.com/afraznein/KTP-ReAPI)** - Custom ReAPI fork
  - Exposes KTP-ReHLDS hooks to AMX plugins
  - Adds `RH_SV_UpdatePausedHUD` constant
  - Bridges engine to plugin layer

**🎮 Plugin Layer:**
- **[KTP Match Handler](https://github.com/afraznein/KTPMatchHandler)** - Match management
  - Pause system with real-time countdown
  - Discord integration via relay
  - Match workflow (ready-up, live, pause)
  - Uses `RH_SV_UpdatePausedHUD` for HUD

- **[KTP Cvar Checker](https://github.com/afraznein/KTPCvarChecker)** - Anti-cheat
  - Real-time client cvar enforcement
  - FTP screenshot upload
  - Player compliance tracking

**🌐 Supporting Services:**
- **[KTP Discord Relay](https://github.com/afraznein/DiscordRelay)** - HTTP relay for Discord
- **[KTP HLTV Kicker](https://github.com/afraznein/KTPHLTVKicker)** - HLTV spectator management

### **Upstream Projects:**
- **[ReHLDS (Upstream)](https://github.com/rehlds/rehlds)** - Original ReHLDS project
- **[ReAPI](https://github.com/rehlds/ReAPI)** - AMX Mod X module for ReHLDS hooks
- **[AMX Mod X](https://www.amxmodx.org/)** - Half-Life scripting platform

---

## 📚 Documentation

**KTP Stack Documentation:**
- [KTP Match Handler - Discord Guide](https://github.com/afraznein/KTPMatchHandler/blob/main/DISCORD_GUIDE.md) - Complete stack overview
- [KTP-ReAPI README](https://github.com/afraznein/KTP-ReAPI/blob/main/README.md) - Module integration
- [KTP Match Handler README](https://github.com/afraznein/KTPMatchHandler/blob/main/README.md) - Plugin usage

**Upstream Documentation:**
- [ReHLDS Wiki](https://github.com/rehlds/ReHLDS/wiki)
- [ReAPI Documentation](https://github.com/rehlds/ReAPI/wiki)
- [AMX Mod X Documentation](https://www.amxmodx.org/api/)

---

## 🙏 Acknowledgments

**KTP Fork:**
- **Nein_** ([@afraznein](https://github.com/afraznein)) - KTP-ReHLDS fork maintainer
- **KTP Community** - Testing, feedback, competitive insights

**Upstream ReHLDS:**
- **dreamstalker** - Original ReHLDS reverse engineering
- **s1lentq** - ReHLDS development and maintenance
- **ReHLDS Contributors** - Bug fixes and enhancements
- **Valve Software** - Original GoldSrc engine

---

## 📝 License

**MIT License** - Same as upstream ReHLDS (transitioned from GPL v3 in July 2025)

See [LICENSE](https://github.com/rehlds/rehlds/blob/master/LICENSE) for full text.

---

## 🤝 Contributing

### For KTP-Specific Features

This is a specialized fork for **KTP competitive infrastructure**.

**KTP contributions welcome:**
- New engine hooks for competitive features
- Performance optimizations for pause system
- Bug fixes for KTP-specific code
- Documentation improvements

**Submit issues/PRs at:**
- https://github.com/afraznein/KTP-ReHLDS/issues

### For General ReHLDS Features

For **general ReHLDS improvements** (not KTP-specific):
- **[Upstream ReHLDS](https://github.com/rehlds/ReHLDS)**

We periodically sync with upstream to incorporate improvements.

---

## 💬 Support

**For KTP-ReHLDS help:**
- Open an issue: https://github.com/afraznein/KTP-ReHLDS/issues
- Check KTP Match Handler docs: https://github.com/afraznein/KTPMatchHandler
- Review KTP-ReAPI docs: https://github.com/afraznein/KTP-ReAPI

**For general ReHLDS questions:**
- Upstream docs: https://github.com/rehlds/ReHLDS
- ReHLDS community forums

---

## 🐛 Troubleshooting

### HUD Updates Not Working

**Problem:** Pause HUD shows static message, no countdown

**Solutions:**
- ✅ Verify KTP-ReHLDS installed (check version in console)
- ✅ Verify KTP-ReAPI loaded (`meta list`)
- ✅ Verify KTPMatchHandler loaded (`amxx list`)
- ✅ Check `pausable 0` in server.cfg
- ✅ Check plugin uses `rh_set_server_pause()` not `pause` command

### Chat During Pause Not Working

**Problem:** Player chat doesn't display during pause

**Solutions:**
- ⚠️ **Player chat is WIP** - not fully implemented yet
- ✅ `rcon say` works (use for announcements)
- ✅ Server events (join/leave) work automatically
- ✅ Plugin messages via `client_print()` work

### Server Crashes on Pause

**Problem:** Server crashes when pause is triggered

**Solutions:**
- ✅ Verify KTP-ReHLDS build matches your HLDS version
- ✅ Check you're using KTP-ReAPI (not standard ReAPI)
- ✅ Verify no conflicting plugins using engine pause
- ✅ Check server logs for error messages

### Performance Issues

**Problem:** Server lags during pause

**Solutions:**
- ✅ Hook called every frame (~60-100 Hz) - keep HUD updates lightweight
- ✅ Avoid complex calculations in `OnPausedHUDUpdate()`
- ✅ Cache countdown strings instead of recalculating every frame
- ✅ Limit message buffer size

---

**KTP-ReHLDS** - The engine foundation that makes professional competitive matches possible. 🎮
