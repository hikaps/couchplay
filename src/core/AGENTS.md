# AGENTS.md - Core Module Guidelines

## OVERVIEW
Core business logic layer: device management, session orchestration, player/sandbox/monitor/audio management. Everything runs unprivileged as the desktop user; there is no root helper.

## STRUCTURE
**Managers and services:**
- `SessionRunner` - Orchestrates multiple GamescopeInstance launches (host-tool check → pre-hook → pad forwarders → session resources → displays → instances)
- `DeviceManager` - Input device detection from /proc/bus/input/devices, hotplug state machine; ignores `CouchPlay ` virtual pads
- `PlayerStore` - Player directories (`players/<name>/{home,layers}`), name validation, create/delete
- `SandboxCommand` - Pure builder of the `bwrap → dbus-run-session → gamescope` argv (`SandboxSpec`, `SandboxMount`)
- `PadForwarder` - Wraps the host `couchplay-padfwd` process (one virtual Xbox 360 pad per player); attach/detach sources, chord/lost signals
- `PlayerFiles` - `copyTreeContents` / `replaceTree` (symlink-safe copies into player homes)
- `GamescopeInstance` - Gamescope arg building and launching the sandbox command; finds the gamescope PID by `COUCHPLAY_SANDBOX_ID`
- `HostProcess` - Runs commands on the host from Flatpak (`flatpak-spawn --host`), `hostFindExecutable`, `hostLibexecPath` (Flatpak app-path tools)
- `SteamConfigManager` / `HeroicConfigManager` - Launcher config discovery; write VDF/catalog/shortcut files directly into player homes
- `SteamCompatToolMapping` - Compat-tool mapping edits for Steam `config.vdf`
- `PresetManager` - Launch preset profiles, .desktop scanning
- `SessionManager` - Session profiles (KConfig), instance configuration (`InstanceConfig::player`)
- `WindowManager` - Window positioning via KWin (QML_SINGLETON)
- `CommandVerifier` - Static utility: command validation, Flatpak detection, PATH resolution
- `SettingsManager` - KConfig-based app settings
- `VirtualDeviceWatcher` - Virtual device monitoring during sessions
- `AudioManager` - PipeWire/PulseAudio audio routing checks
- `MonitorManager` - Display detection via QGuiApplication screens
- `StreamManager`, `StreamAudio`, `SunshineConfig`, `PairingSession` - Sunshine streaming per player

**Data Structures:** `InputDevice`, `InstanceConfig`, `SessionProfile`, `SteamPaths`, `SteamShortcut` (Q_GADGET structs)

**Shared Utilities:**
- `Logging.h/cpp` - scoped logging categories (see the file for the current list)
- `CommandVerifier.h/cpp` - Static utility for command validation (Flatpak detection, PATH resolution, executable checks)

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Device hotplug | `DeviceManager::onInputDirectoryChanged()` | QFileSystemWatcher + debounce |
| Stable device IDs | `DeviceManager::generateStableId()` | vendorId:productId:physPath |
| Device reconnection | `SessionRunner::onDeviceReconnected()` | Re-attaches the source to the player's virtual pad |
| Layout calculations | `SessionRunner::calculateLayout()` | horizontal/vertical/grid/multi-monitor |
| Sandbox argv | `buildSandboxArgv()` in `SandboxCommand.cpp` | Ordering invariants are unit tested |
| Gamescope args | `GamescopeInstance::buildGamescopeArgs()` | Constructs gamescope command line |
| Player paths | `PlayerStore::homeDir()` / `layerDir()` | Names validated with `isValidName` |
| VDF parsing | `SteamConfigManager::parseShortcutsVdf()` | Steam format parser |
| Profile persistence | `SessionManager::saveProfile()` / `loadProfile()` | KConfig `.conf` files in `AppDataLocation/profiles` (legacy `username` key read as `player`) |
| Audio routing | `AudioManager::checkConfiguration()` | Detects PipeWire/PulseAudio |

## CONVENTIONS

**Manager Pattern:** QObject → QML_ELEMENT, Q_PROPERTY for data, Q_INVOKABLE for actions, dependency injection with `*Changed` signals, emit `errorOccurred(QString)` for errors

**Struct Pattern (Q_GADGET):** Plain data structs, Q_PROPERTY with MEMBER, Q_DECLARE_METATYPE for QVariant conversion, no logic

## ANTI-PATTERNS

- No privileged operations and no root helper: everything runs as the desktop user inside the sandbox
- No unvalidated player names in paths: use `PlayerStore::isValidName`
- No shell interpolation: pass structured argv through `HostProcess::configure`
- No blocking I/O in main thread (known synchronous exceptions: host probes, `PadForwarder::start`, `GamescopeInstance::stop`)
- No hardcoded paths: use QStandardPaths / `PlayerStore`
- No QML logic: business logic in C++ only
- No circular dependencies: SessionRunner → SessionManager (unidirectional)

## ARCHITECTURE NOTES

**Dependency Graph:**
```
SessionRunner (orchestrator)
  ├─> SessionManager, DeviceManager, PlayerStore, WindowManager
  ├─> GamescopeInstance (N) ──> SandboxCommand (bwrap → dbus-run-session → gamescope)
  ├─> PadForwarder (N) ──> couchplay-padfwd (host tool)
  └─> SteamConfigManager, HeroicConfigManager (write into player homes)
```

Test seams: `SessionRunner::setInstanceFactory` (fake `GamescopeInstance`) and `PadForwarder::setProgramOverrideForTesting`.

**Device Assignment Flow:** DeviceManager detects → stableId → user assigns via QML → SessionRunner starts a `PadForwarder` per instance → controller event nodes are `attach`ed to the player's virtual pad (non-controller nodes are bound directly into the sandbox) → hotplug reconnection re-attaches.

## COMPLEXITY HOTSPOTS

**DeviceManager.cpp:**
- `onDebounceTimeout()`: 4-phase hotplug state machine (store old → parse new → detect disconnection → detect reconnection)
- `parseDevices()`: Regex-based `/proc/bus/input/devices` parser
- `detectDeviceType()`: Ghost device filtering (opens device, queries EVIOCGBIT)

**SessionRunner.cpp:**
- `start()`: validation (player exists, host tools present) → layout → pad forwarders → session resources/mounts → displays → instance creation
- `setupSessionResources()`: per-player Steam/Heroic sync and data-directory modes (`acl` read-only bind, `bind`, `overlay`, `copy`)

**SteamConfigManager.cpp:**
- `parseShortcutsVdf()`: Binary VDF parser with type markers (0x00=object, 0x01=string, 0x02=int32)
