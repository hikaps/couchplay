# AGENTS.md - Core Module Guidelines

## OVERVIEW
Core business logic layer: device management, session orchestration, user/monitor/audio management

## STRUCTURE
**14 Manager Classes:**
- `DeviceManager` (954 lines) - Input device detection from /proc/bus/input/devices
- `SessionRunner` (823 lines) - Orchestrates multiple GamescopeInstance launches
- `SessionManager` - Session profiles, instance configuration
- `GamescopeInstance` - Gamescope process wrapper with arg building
- `UserManager` - Linux user creation/management
- `GameLibrary` - Steam game detection
- `MonitorManager` - Display detection via RandR
- `AudioManager` - PipeWire audio routing
- `SettingsManager` - KConfig-based app settings
- `PresetManager` - Launch preset profiles
- `WindowManager` - Window positioning via KWin
- `SteamConfigManager` (599 lines) - VDF parsing, shortcuts syncing
- `CommandVerifier` - Steam game command detection
- `Logging` - Application logging

**Data Structures:** `InputDevice`, `InstanceConfig`, `SessionProfile`, `SteamPaths`, `SteamShortcut` (Q_GADGET structs)

**Shared Utilities:**
- `Logging.h/cpp` - 6 scoped logging categories (couchplayCore, couchplaySteam, couchplayHelper, couchplayGamescope, couchplayDevices, couchplaySharing)
- `CommandVerifier.h/cpp` - Static utility for command validation (Flatpak detection, PATH resolution, executable checks)
- `../dbus/CouchPlayHelperClient` - Shared D-Bus client for privileged operations (used by SessionRunner, UserManager, SteamConfigManager, GamescopeInstance)

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Device hotplug | `DeviceManager::onInputDirectoryChanged()` | QFileSystemWatcher + debounce |
| Stable device IDs | `DeviceManager::generateStableId()` | vendorId:productId:physPath |
| Device reconnection | `SessionRunner::onDeviceReconnected()` | Auto-restores ownership |
| Layout calculations | `SessionRunner::calculateLayout()` | horizontal/vertical/grid/multi-monitor |
| Gamescope args | `GamescopeInstance::buildGamescopeArgs()` | Constructs command line |
| VDF parsing | `SteamConfigManager::parseShortcutsVdf()` | Steam format parser |
| Profile persistence | `SessionManager::saveProfile()` / `loadProfile()` | JSON in ~/.local/share/couchplay/profiles/ |

## CONVENTIONS

**Manager Pattern:** QObject → QML_ELEMENT, Q_PROPERTY for data, Q_INVOKABLE for actions, dependency injection with `*Changed` signals, emit `errorOccurred(QString)` for errors

**Struct Pattern (Q_GADGET):** Plain data structs, Q_PROPERTY with MEMBER, Q_DECLARE_METATYPE for QVariant conversion, no logic

**Signal Naming:** Past tense state changes (`devicesChanged()`), event-based (`deviceAssigned()`), `errorOccurred(QString message)`

**Member Variables:** `m_` prefix, pointers = `nullptr`, bools = `false`

## ANTI-PATTERNS

- No direct D-Bus: use `CouchPlayHelperClient` in `../dbus/`
- No privileged ops: all root work in helper/ service
- No blocking I/O in main thread
- No hardcoded paths: use QStandardPaths (~/.local/share/couchplay/)
- No QML logic: business logic in C++ only
- No circular dependencies: SessionRunner → SessionManager (unidirectional)
- No direct process management: use `QProcess` via `GamescopeInstance`
- No manual signal/slot connects: prefer Qt5 connect syntax

## ARCHITECTURE NOTES

**Dependency Graph:**
```
SessionRunner (orchestrator)
  ├─> SessionManager, DeviceManager, GamescopeInstance (N)
  ├─> UserManager, WindowManager
  └─> SteamConfigManager, CouchPlayHelperClient
```

**Device Assignment Flow:** DeviceManager detects → stableId → user assigns via QML → SessionRunner starts → Helper transfers ownership via D-Bus → hotplug reconnection → auto-restore

## COMPLEXITY HOTSPOTS

**DeviceManager.cpp (954 lines):**
- `onDebounceTimeout()` (lines 72-181): 4-phase hotplug state machine (store old → parse new → detect disconnection → detect reconnection)
- `parseDevices()` (lines 190-340): Regex-based `/proc/bus/input/devices` parser
- `detectDeviceType()`: Ghost device filtering (opens device, queries EVIOCGBIT)

**SessionRunner.cpp (837 lines):**
- `start()` (lines 145-300): 6-phase orchestration (validation → layout → device ownership → mounts → ACLs → instance creation)
- `setupLauncherAccess()`: Conditional ACL + Steam shortcuts sync

**SteamConfigManager.cpp (599 lines):**
- `parseShortcutsVdf()`: Binary VDF parser with type markers (0x00=object, 0x01=string, 0x02=int32)
