# Repository Guidelines

CouchPlay is a C++20/QML/Qt6/KF6/Kirigami application for **split-screen gaming on Linux** (Wayland/KDE). It launches multiple `gamescope` instances per monitor, assigns input devices per player, and optionally streams a session via **Sunshine**. A privileged D-Bus helper (`couchplay-helper`, root) handles user creation, device ownership, virtual displays, and process launching.

## Architecture & Data Flow

```
QML UI (Kirigami)  ←→  Core Managers (C++)  ←→  D-Bus Helper (root)
  SessionSetupPage       SessionManager           CouchPlayHelper
  DeviceAssignment       SessionRunner             (LaunchInstance, CreateUser,
  HomePage               GamescopeInstance          ChangeDeviceOwner,
  SettingsPage           StreamManager              CreateVirtualOutput,
                         SunshineConfig             CreateNullSink, ...)
```

**Session start flow:**
1. `SessionManager` holds the `SessionProfile` (layout, `InstanceConfig` list).
2. `SessionRunner::start()` splits instances into physical vs streaming.
3. **Physical**: `setupDeviceOwnership` → `GamescopeInstance::start()` → helper `LaunchInstance(username, compositorUid, gamescopeArgs, gameCommand, ...)` → gamescope renders to a monitor region.
4. **Streaming**: `setupStreamingInstance()` → helper `CreateVirtualOutput(username, W, H, RR)` (gamescope as virtual Wayland compositor) + `CreateNullSink(username, sinkName)` (PipeWire) → `StreamManager::startStream()` → `SunshineConfig::generateConfig()` writes `/tmp/couchplay-sunshine-<n>/{sunshine.conf,apps.json,credentials.json}` → helper `LaunchInstance(username, ..., "sunshine <configPath>")`.
5. `WindowManager` positions physical gamescope windows via KWin D-Bus scripting.
6. `VirtualDeviceWatcher` attributes new input devices (Steam Input, Sunshine virtual) to the correct user via FD inspection.

**Key interface**: `CouchPlayHelperClient` (`src/dbus/`) is the GUI-side D-Bus proxy; `CouchPlayHelper` (`helper/`) is the root-side implementation. Every privileged operation goes through this pair.

## Key Directories

| Path | Purpose |
|---|---|
| `src/core/` | 17 manager classes (~10K lines) — session orchestration, device management, streaming, window positioning |
| `src/qml/` | Kirigami UI (pages, components, dialogs) — packaged as `io.github.hikaps.couchplay` QML module via `ecm_add_qml_module` |
| `src/dbus/` | `CouchPlayHelperClient` — D-Bus proxy for the privileged helper |
| `helper/` | Privileged D-Bus service (`couchplay-helper`, ~2.4K lines) — runs as root; user/device/process management |
| `tests/` | 21 QtTest executables registered with CTest; tests `#include` sources directly |
| `data/` | Polkit policy (11 actions), D-Bus configs/services, systemd unit, desktop/metainfo/icons, PipeWire config |
| `scripts/` | `install.sh` (one-liner), `install-helper.sh`, `bundle-libs.sh` ($ORIGIN RPATH), `run-debug.sh` |

**Per-directory guides**: deeper detail lives in `src/core/AGENTS.md`, `src/qml/AGENTS.md`, `helper/AGENTS.md`, and `tests/AGENTS.md`.

## Development Commands

```bash
# Configure + build (host or dev container)
cmake -B build
cmake --build build -j$(nproc)

# Run the app (must be on host — gamescope needs the real display)
./build/bin/couchplay

# QtTest suite (21 executables; run under dbus-run-session for D-Bus tests)
QT_QPA_PLATFORM=offscreen dbus-run-session -- ctest --test-dir build --output-on-failure

# Single test
QT_QPA_PLATFORM=offscreen dbus-run-session -- ./build/bin/test_streammanager

# Lint (targets defined in root CMakeLists.txt; NOT enforced in CI)
make format    # clang-format (WebKit, 120-char, C++20)
make tidy      # clang-tidy (diagnostic/analyzer/performance/bugprone/modernize/...)

# Debug launch with logging categories
./run-debug.sh --all       # all Qt debug logs
./run-debug.sh --helper    # D-Bus helper logs only

```

## Code Conventions & Common Patterns

### C++ Headers
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors
```

### Include Order
1. Own header (`.cpp` files)
2. Qt headers (alphabetical)
3. KDE Frameworks headers
4. System headers (`<unistd.h>`, `<pwd.h>`)
5. Project headers

### Naming
- **Classes**: PascalCase (`DeviceManager`, `StreamManager`)
- **Methods**: camelCase (`buildGamescopeArgs()`, `setupStreamingInstance()`)
- **Member vars**: `m_camelCase` (`m_sessionManager`, `m_streams`)
- **Qt signals**: camelCase, past tense (`devicesChanged()`, `sessionStarted()`)
- **Qt slots**: `on` + Source + Event (`onProcessStarted()`)

### Qt/KDE Idioms
- `QStringLiteral()` for all string literals (no runtime alloc)
- `Q_EMIT` (not bare `emit`); `Q_SIGNALS`/`Q_SLOTS` (not `signals`/`slots`)
- `#pragma once`; `nullptr` (not `NULL`); `override` on virtuals
- `Q_PROPERTY(... MEMBER ...)` for QML-exposed fields
- `Q_INVOKABLE` on methods called from QML

### QML/Kirigami
- Import with aliases: `import org.kde.kirigami as Kirigami`
- `i18nc("@context", "string")` for all user-visible text
- `objectName: "controlName"` for stable control identification
- PascalCase filenames (`SessionSetupPage.qml`)
- Required properties for mandatory injections

### Error Handling
- `qWarning()` — recoverable errors (logged, continues)
- `qDebug()` — development output only (not shown in release)
- `Q_EMIT errorOccurred(QString)` — user-facing errors (shown in UI)
- Helper: `sendErrorReply(QDBusError::Failed, ...)` for D-Bus errors

### Testing Patterns
- **Unit tests** use `#define private public` to access internals (no mocking framework)
- `MockSystemOps` (25 virtual overrides) injects into `CouchPlayHelper` for helper tests
- `MockCouchPlayHelperClient` subclass for SessionRunner tests
- Tests `#include` source `.cpp` files directly (not a linked library) — deliberate trade-off documented in AGENTS.md

### Helper Privileged Patterns
- `validateUserAndAuth(username, action)` — 3-check gate (username regex, user exists, Polkit auth) before every privileged op
- `runCommand(program, args, timeout)` — shared QProcess spawn/await pattern
- D-Bus args pass through `execve` (no shell) — no command injection

## Important Files

| File | Role |
|---|---|
| `src/main.cpp` | App entry point; creates managers, loads QML module |
| `src/qml/Main.qml` | QML entry; instantiates all managers, global drawer actions |
| `src/core/SessionRunner.cpp` | Session lifecycle orchestrator (~1.5K lines); physical + streaming instance management |
| `src/core/StreamManager.cpp` | Sunshine subprocess lifecycle; crash recovery (`MAX_RESTART_ATTEMPTS=3`), startup timeout, port-bump on crash |
| `src/core/SunshineConfig.cpp` | Generates per-instance `sunshine.conf` / `apps.json` / `credentials.json`; port = `47989 + index×30` |
| `helper/CouchPlayHelper.cpp` | Root D-Bus service (~2.4K lines); user/device/virtual-display/null-sink/process management |
| `data/polkit/io.github.hikaps.couchplay.policy` | 11 Polkit actions (gates `CreateUser`, `DeleteUser`, `CreateVirtualOutput`, `CreateNullSink`, etc.) |

## Runtime/Tooling Preferences

- **OS**: Developed on Bazzite (immutable Fedora, Wayland/KDE Plasma). The app **must run on the host** for real gamescope/display interaction.
- **Build deps**: CMake ≥ 3.20, C++20, Qt6 ≥ 6.5 (Core/Quick/Qml/Gui/QuickControls2/Widgets/DBus), KF6 ≥ 6.0 (Kirigami/I18n/CoreAddons/Config/IconThemes/QQC2DesktopStyle/GlobalAccel), ECM ≥ 6.0, PolkitQt6-1, PipeWire-devel, dbus-daemon.
- **Package manager**: system `dnf` (Fedora); no npm/cargo/pip for the app itself.
- **Formatting**: `.clang-format` (WebKit base, 120-char, C++20, Linux braces, right pointers). `.editorconfig` (4-space C++/QML/CMake/sh, 2-space JSON/YAML, tab Makefile).
- **App ID**: `io.github.hikaps.couchplay` (D-Bus service name, QML module URI, desktop file, Flatpak ID).

## Testing & QA

### Unit Tests (CI gate)
- **Framework**: QtTest (`QTest::qExec`), 21 test executables.
- **Run**: `QT_QPA_PLATFORM=offscreen dbus-run-session -- ctest --test-dir build --output-on-failure`
- **CI**: `.github/workflows/ci.yml` runs all 21 on push/PR to `develop` (Fedora 41 container, no exclusions).
- **Coverage**: managers (DeviceManager, SessionManager, SessionRunner, GamescopeInstance, StreamManager, SunshineConfig, WindowManager, AudioManager, UserManager, PresetManager, etc.) + the full helper (test_couchplayhelper, 62 tests via MockSystemOps).
- **Test source inclusion**: tests `#include` the `.cpp` sources directly (not a linked library). Each test target compiles its own copy of the core sources. This is a deliberate trade-off (documented anti-pattern).

### Manual release smoke
- Verify profile save/load/duplication and Steam registration in the real QML application.
- On the Linux host with the helper, verify physical-session input/audio isolation and cleanup; with Sunshine and Moonlight, verify a real streaming session and teardown.
- Hosted CI runs the QtTest suite and an offscreen application smoke. It does not exercise the real desktop, helper-backed sessions, or streaming.

### Branching & Release
- **`develop`**: integration branch; all PRs merge here.
- **`main`**: stable releases only. Tag + release from `main`, never from `develop`.
- **Branch prefixes**: `feature/`, `feat/`, `fix/`, `ci/`, `chore/`, `remove/`.
- **CI workflows**: `ci.yml` (tests on develop), `beta.yml` (rolling pre-release from develop), `release.yml` (tagged release from main), `flatpak.yml` (Flatpak bundle from main tag).

## Known Limitations & Anti-Patterns

- **Blocking sleep**: `src/core/WindowManager.cpp` uses `QThread::msleep(100)` for window positioning (a short blocking wait) rather than an async signal.
- **Missing i18n infrastructure**: `KF6::I18n` is linked and `i18nc(...)` is used in QML, but there is no `po/` directory or translation files — strings are not currently shipped translated.
- **No CI linting**: `make format` / `make tidy` targets exist but are not enforced in CI.

## SteamOS / Deck Rules

- **Platform Check**: Determine if running on SteamOS by checking:
  `grep -q "ID=steamos" /etc/os-release`
- **Rebuilding & Updating**: When running on SteamOS, run the local user-space updater script `scripts/update-nonroot.sh` every time you build a new version of the application.
