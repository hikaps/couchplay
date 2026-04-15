# AGENTS.md - Agent Guidelines for CouchPlay

**C++20/QML KDE/Qt6 Kirigami application for split-screen gaming on Linux (GPL-3.0-or-later)**

## Build Environment

Developed on **Bazzite** (immutable Fedora with Wayland/KDE). Build directly on the host or in a development container.

```bash
# Configure (run from project root)
cmake -B build

# Build
cmake --build build

# Run (on HOST - gamescope requires host environment)
./build/bin/couchplay

# Tests
ctest --test-dir build --output-on-failure

# Single test: ctest --test-dir build -R DeviceManagerTest --output-on-failure
# List tests: ctest --test-dir build -N
# Direct run: ./build/bin/test_devicemanager
```

## Structure

```
./
├── src/core/       # 15 manager classes (30 files, ~10.5K lines) - SEE ./src/core/AGENTS.md
├── src/qml/        # UI layer (pages + components) - SEE ./src/qml/AGENTS.md
├── helper/         # Privileged D-Bus service (CouchPlayHelper.cpp 1945 lines) - SEE ./helper/AGENTS.md
├── tests/          # QtTest unit tests (14 files, ~9.6K lines) - SEE ./tests/AGENTS.md
├── src/dbus/       # D-Bus client for helper service
└── data/           # Icons, polkit policy, D-Bus service files
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Manager architecture | `./src/core/AGENTS.md` | DeviceManager, SessionManager, etc. |
| QML layer | `./src/qml/AGENTS.md` | Kirigami components, page patterns |
| Test patterns | `./tests/AGENTS.md` | Test naming, fixtures, mocking |
| Privileged helper | `./helper/AGENTS.md` | D-Bus service, user mgmt, device ownership |
| Device detection | `src/core/DeviceManager.{cpp,h}` | Parses `/proc/bus/input/devices` |
| Session orchestration | `src/core/SessionRunner.{cpp,h}` | Starts/stops multiple GamescopeInstance |
| Gamescope wrapping | `src/core/GamescopeInstance.{cpp,h}` | Argument building + D-Bus launch |
| D-Bus client | `src/dbus/CouchPlayHelperClient.{cpp,h}` | Communicates with helper service |
| QML entry point | `src/qml/Main.qml` | Creates all manager instances |
| Privileged actions | `data/polkit/io.github.hikaps.couchplay.policy` | Polkit action definitions |

## CODE MAP (Core Managers)

| Manager | Purpose | Key Signals |
|---------|---------|-------------|
| DeviceManager | Input device detection/assignment | `deviceAssigned`, `devicesChanged`, `deviceReconnected` |
| SessionManager | Session profiles, instance config | `currentLayoutChanged`, `instancesChanged`, `profileLoaded` |
| GamescopeInstance | Gamescope launch (args + D-Bus) | `started`, `stopped`, `configChanged`, `statusChanged` |
| SessionRunner | Orchestrates multiple instances | `sessionStarted`, `sessionStopped`, `instanceStarted` |
| UserManager | Linux user management | `usersChanged`, `userCreated` |
| MonitorManager | Display detection | `monitorsChanged` |
| AudioManager | PipeWire configuration | - |

## CONVENTIONS (Deviations from Standard C++/Qt)

### File Headers
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors
```

### Include Order
1. Own header (.cpp files)
2. Qt headers (alphabetical)
3. KDE Frameworks headers
4. System headers
5. Project headers

### Naming
- Classes: PascalCase (`DeviceManager`)
- Member vars: `m_camelCase`
- Methods: camelCase (`buildGamescopeArgs()`)
- Qt signals: camelCase, past tense (`devicesChanged()`)
- Qt slots: `on + Source + Event` (`onProcessStarted()`)
- QML files: PascalCase (`HomePage.qml`)

### Qt/KDE Specific
- Use `QStringLiteral()` for string literals (no runtime alloc)
- Use `Q_EMIT` instead of `emit`
- Use `Q_SIGNALS`/`Q_SLOTS` instead of `signals`/`slots`
- Use `#pragma once` instead of include guards
- Use `nullptr` instead of `NULL` or `0`
- Use `override` for virtual methods

### QML/Kirigami
- Import with aliases: `import org.kde.kirigami as Kirigami`
- Use `i18nc()` for user-visible strings with context
- Component IDs: camelCase (`id: deviceManager`)
- Properties: `required property` for mandatory injections

### Class Declaration Order
1. Q_OBJECT macro
2. QML_ELEMENT (if exposed to QML)
3. Q_PROPERTY declarations
4. public: constructors/destructor
5. public: Q_INVOKABLE methods
6. public: getters/setters
7. Q_SIGNALS:
8. public/private Q_SLOTS:
9. private: helpers
10. private: member variables

### Error Handling
- `qWarning()` for recoverable errors
- `qDebug()` for development output only
- Emit `errorOccurred(QString)` for user-facing errors
- Clean up in destructors

### Extracted Helpers (helper/)
- `validateUserAndAuth()` — shared 3-check pattern for D-Bus slots (username validity, user exists, Polkit auth)
- `runCommand()` — shared QProcess spawn/await pattern (start, waitForFinished, return output)

## ANTI-PATTERNS (Project-Specific)

- **No linting config**: No `.clang-format`, `.clang-tidy`, `.editorconfig`
- **Test source inclusion**: Tests include source files directly instead of linking targets (see `tests/CMakeLists.txt`)
- **Gamescope host requirement**: App must run on host, not in container (gamescope needs host display)
- **Incomplete Polkit**: Helper service has `TODO: Implement proper PolicyKit authorization check`
- **Missing i18n infrastructure**: `KF6::I18n` linked but no `po/` directory or translation files exist

## UNIQUE PATTERNS

### Modular QML Architecture
- Uses `ecm_add_qml_module` to package QML as a module (`io.github.hikaps.couchplay 1.0`)
- Loads QML with `engine.loadFromModule()` instead of `load("qrc:/Main.qml")`

### D-Bus Helper Pattern
- GUI (`couchplay`) communicates with privileged helper (`couchplay-helper`) via D-Bus
- Helper uses Polkit for authorization (see `data/polkit/io.github.hikaps.couchplay.policy`)
- Performs device ownership transfer, user creation, PipeWire configuration

### Device Stable IDs
- DeviceManager generates stable device IDs from physical path info (vendor/product IDs, phys path)
- Enables device reconnection recognition after USB hotplug
- See `DeviceManager::generateStableId()` implementation

## BRANCHING & RELEASE

- **develop**: Main development branch, all PRs merge here
- **main**: Stable releases only
- **Feature branches**: Pattern `feature/<feature-name>`
- **Releases**: Tag and release only from `main`, never from `develop`. Merge develop into main, tag, push.

## NOTES

- **App ID**: `io.github.hikaps.couchplay` (D-Bus, QML module, desktop file)
- **Local docs**: `PLAN.md` has architecture overview and feature roadmap
- **Build artifacts**: Ignore `build/` directory
- **Root test files**: `test_*.cpp` are temporary/experimental, not part of test suite
- **CI exclusions**: CI skips 7/14 tests requiring D-Bus/Polkit/devices (see `.github/workflows/ci.yml`)
- **Security TODO**: `helper/SystemOps.cpp:checkAuthorization()` stub returns true (Polkit not implemented)

## GIT META

- **Commit**: `8d9a171`
- **Branch**: `fix/ui`
