# Repository Guidelines

## Project Overview

CouchPlay is a C++20/Qt6/KF6/Kirigami application for split-screen gaming on Linux, primarily KDE Plasma/Wayland. It runs per-player Gamescope instances, assigns controllers and audio, supports native/Flatpak launchers, and optionally streams through Sunshine. A root D-Bus helper performs privileged host operations; the GUI remains unprivileged.

## Architecture & Data Flow

```text
Kirigami QML → C++ managers → CouchPlayHelperClient → system D-Bus → CouchPlayHelper
                                  GUI process                       root process
```

- `src/main.cpp` initializes Qt/QML, parses profile-launch arguments, and handles single-instance activation through `CommandLineBridge`. `src/qml/Main.qml` constructs managers, injects dependencies, and connects UI notifications.
- `SessionManager` owns editable `SessionProfile`/`InstanceConfig` values. Profiles are KConfig `.conf` files below `QStandardPaths::AppDataLocation/profiles`, not JSON. Persist stable device IDs; runtime input-event numbers are separate.
- `PresetManager` resolves launcher/game selections into `LaunchCommand` (`program`, argv, working directory). Steam/Heroic config managers handle launcher-specific discovery and data; generic applications come from desktop entries/custom presets.
- `SessionRunner` validates and snapshots the starting profile, runs an optional pre-hook, prepares users/devices/data mounts, and launches instances sequentially. Physical instances use `GamescopeInstance` plus KWin window positioning through `WindowManager`.
- Streaming instances prepare a virtual output and audio sink; after Gamescope starts, `StreamManager` generates Sunshine configuration through `SunshineConfig` and launches Sunshine through the helper. Startup timers and bounded restart handling belong to `StreamManager`.
- Stop/startup-failure paths restore device ownership and tear down instances, streams, mounts, outputs, and audio before completing the post-hook/final state transition. Preserve this centralized cleanup when changing launch flow.
- `SteamShortcutManager` registers saved profiles as native/Flatpak Game Mode shortcuts. `SteamShortcutsVdf` preserves unrelated binary VDF entries; registration checks paths/concurrent changes, creates a backup, and writes atomically.
- Privileged calls go through `src/dbus/CouchPlayHelperClient.*` and matching helper slots. D-Bus service/object: `io.github.hikaps.CouchPlayHelper`, `/io/github/hikaps/CouchPlayHelper`. Keep client, helper, bus policy, and Polkit actions consistent.

## Key Directories

| Path | Assistant-relevant purpose |
|---|---|
| `src/core/` | Session lifecycle, launcher/config discovery, device/audio/display management, streaming, Steam registration |
| `src/qml/` | Kirigami pages/components/dialogs; QML module `io.github.hikaps.couchplay` |
| `src/dbus/` | GUI-side privileged-helper proxy |
| `helper/` | Root service; `SystemOps` injection seam and `SecureFs` filesystem/mount protections |
| `tests/` | QtTest executables and explicit CTest target registration |
| `data/` | System D-Bus policy/activation/unit, Polkit actions, PipeWire config, desktop/metainfo/icons |
| `scripts/` | Dependency/bootstrap, installation, library bundling, SteamOS updates, Game Mode launcher |
| `.github/workflows/` | Hosted CI, rolling beta, stable release, and Flatpak packaging |

Directory guides exist in `src/core/`, `src/qml/`, `helper/`, and `tests/`. Check their descriptions against current source/CMake: older guides contain stale persistence, authorization, and test-build claims.

## Development Commands

Run from the repository root on Linux or in a compatible build container:

```bash
# Fedora dependency bootstrap (uses dnf/root privileges)
./scripts/install-build-deps.sh dbus-daemon

cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel 2

# Real display/session work: run on the Linux host
./build/bin/couchplay
./build/bin/couchplay --profile "My Profile" --start --exit-after-session
./run-debug.sh --core --profile "My Profile" --start
./run-debug.sh --helper

# Optional targets: tools must be installed when CMake configures
cmake --build build --target format  # clang-format edits files in place
cmake --build build --target tidy   # clang-tidy uses the compile database
```

Run the full suite with a private session bus and isolated runtime directory. Audio tests create/remove `pipewire-0` under `QStandardPaths::RuntimeLocation`; do not point them at a live desktop runtime directory.

```bash
(
    runtime=$(mktemp -d)
    trap 'rm -rf -- "$runtime"' EXIT
    chmod 700 "$runtime"
    export XDG_RUNTIME_DIR="$runtime" QT_QPA_PLATFORM=offscreen
    dbus-run-session -- ctest --test-dir build --output-on-failure
    # For a targeted run, add -R '^test_sessionrunner$' to ctest above.
    # A single binary can also run here:
    # dbus-run-session -- ./build/bin/test_streammanager
)
```

## Code Conventions & Common Patterns

- **Formatting/naming:** `.clang-format` uses WebKit/C++20, four spaces, 120 columns, Linux braces, right-aligned pointers. `.editorconfig` sets LF/final newline, four spaces for C++/QML/CMake/shell and two for JSON/YAML. Classes/QML files use PascalCase; methods use camelCase; members use `m_camelCase`; event handlers commonly use `onProcessStarted()`.
- **C++/Qt:** retain SPDX headers, `#pragma once`, `nullptr`, `override`, `QStringLiteral`, `Q_SIGNALS`/`Q_SLOTS`, and `Q_EMIT`. Include the implementation's own header first, then Qt/KDE/system/project headers. Reuse neighboring include/layout conventions.
- **QML boundary:** managers are `QObject`/`QML_ELEMENT` types with `Q_PROPERTY` plus notify signals and `Q_INVOKABLE` actions. Value configs use `Q_GADGET`/`Q_PROPERTY(... MEMBER ...)`. Compare before emitting change notifications; avoid duplicating authoritative C++ state in QML.
- **Composition/ownership:** inject managers and `CouchPlayHelperClient` through existing setters/properties; use QML `required property` for mandatory page dependencies. Parent-owned QObjects cover runner-created instances/processes; injected dependencies are not runner-owned. The helper accepts `SystemOps*` and otherwise constructs `RealSystemOps`.
- **State/lifecycle:** `SessionRunner.active` includes startup/hooks/teardown; `running` reflects live instances. Keep these distinct. Use signals, `QTimer`, and `QProcess::finished/errorOccurred` for lifecycle transitions; retain bounded startup/restart behavior and cleanup on failure.
- **Blocking caveat:** helper-client `QDBusInterface::call` and helper process wrappers are synchronous; do not describe the whole launch path as asynchronous. Avoid adding event-loop blocking work. Existing KWin positioning also contains a blocking wait.
- **Errors/logging:** use category-based `qCDebug`/`qCWarning` where available (`Logging.*`), or existing `qDebug`/`qWarning` patterns. User-facing failures emit `errorOccurred(QString)`; helper failures use `sendErrorReply`. Preserve separate startup-failure versus normal-stop signals.
- **QML text/accessibility:** alias Kirigami imports, use `i18nc("@context", "text")` for visible strings, and preserve stable `objectName`/accessible metadata. Required properties and bindings are preferred to hidden global manager lookups.
- **Privileged boundary:** treat helper arguments as untrusted; retain target/caller ownership checks, action-specific Polkit authorization, structured executable/argv passing, environment/path restrictions, and tracked-process ownership. Use `SecureFs` FD-relative/no-follow operations for protected filesystem/mount work; do not replace them with unchecked path-based operations or shell interpolation. See `PolkitActions.h`, `MountSpec.h`, and the matching `data/` policies.

## Important Files

| File | Why to inspect |
|---|---|
| `src/main.cpp`, `src/qml/Main.qml` | CLI/single-instance entry and QML manager composition |
| `src/core/SessionManager.h`, `src/core/LaunchTypes.h` | Profile/instance schema and structured launch values |
| `src/core/SessionRunner.cpp` | Startup, hooks, resource staging, failure handling, teardown |
| `src/core/GamescopeInstance.cpp`, `src/core/WindowManager.cpp` | Gamescope launch arguments and KWin placement |
| `src/core/StreamManager.cpp`, `src/core/SunshineConfig.cpp` | Sunshine lifecycle and generated configuration |
| `src/core/PresetManager.cpp` | Launcher/custom-preset discovery and command construction |
| `src/core/SteamShortcutManager.cpp`, `src/core/SteamShortcutsVdf.cpp` | Safe saved-profile registration in Steam |
| `helper/CouchPlayHelper.cpp`, `helper/SystemOps.h`, `helper/SecureFs.h` | Privileged service, test seam, filesystem invariants |
| `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt` | Dependencies, QML packaging, test source/target lists |
| `.github/workflows/ci.yml`, `io.github.hikaps.couchplay.json` | Actual CI gates and Flatpak runtime/build settings |

## Runtime/Tooling Preferences

- Required build stack: CMake ≥ 3.20, C++20 compiler, Qt ≥ 6.5, KF6/ECM ≥ 6.0, Kirigami QML runtime, and **required** `PolkitQt6-1` (despite older README wording). Tests additionally require Qt Test. Fedora bootstrap installs PipeWire development packages and the Qt/KF components listed in CMake.
- This is a Linux host integration app, not a Node/Bun/Python application. Fedora uses `dnf`; builds/tests may run in a dev container. Real Gamescope/input/audio/KWin checks require the host display and installed privileged helper; offscreen CI cannot substitute for them.
- Native binaries need compatible host Qt/KF libraries. Prefer the Flatpak GUI on Arch/SteamOS rather than a Fedora-built tarball; helper installation still happens on the host. Game Mode launch requires `kwin_wayland` in its runtime. The Flatpak manifest uses KDE Platform/SDK 6.10.
- `scripts/install-helper.sh` handles host D-Bus/Polkit/service installation; `bundle-libs.sh` uses `$ORIGIN` RPATH but deliberately does not bundle glibc/the dynamic linker. `run-debug.sh` consumes its first argument as the logging selector and forwards subsequent arguments to CouchPlay.
- **SteamOS rule:** detect `ID=steamos` in `/etc/os-release`; after every new application build, run `./scripts/update-nonroot.sh`. It rebuilds the local Flatpak and sysext image and prints the privileged refresh/restart commands; it does not apply those commands itself.
- App/Flatpak/QML ID is `io.github.hikaps.couchplay`; the privileged D-Bus service is separately named `io.github.hikaps.CouchPlayHelper`.
- PRs target `develop`; stable tags/releases belong to `main`. `beta.yml` publishes from `develop`. Do not mistake reduced beta/release test selections for the full develop CI gate.

## Testing & QA

- QtTest executables are registered with CTest in `tests/CMakeLists.txt`. Tests compile shared manager/helper source lists into each executable, not a production library and not direct `.cpp` includes. Register new targets through the existing `add_couchplay_test`/`add_helper_test` helpers.
- Follow handwritten test doubles (`MockCouchPlayHelperClient`, `MockSystemOps`), `QTemporaryDir`, scoped environment restoration, and Qt test paths. Some tests use `#define private public`; reuse existing seams rather than adding a mocking framework. Use `QSignalSpy` and bounded `QTRY_*` assertions for async transitions.
- Cover changed observable behavior: profile persistence/migration, launch validation, lifecycle/failure cleanup, config parsing, and ownership/path boundaries. Steam tests use fabricated account/VDF trees; helper tests run an in-process session-bus service with fake system operations, not the real root helper.
- `.github/workflows/ci.yml` runs the full suite on a hosted runner inside Fedora 41, then launches the real app offscreen/software with isolated HOME/XDG paths and timeouts: `--help` succeeds; `--start` without a profile exits 2; an absent profile exits 1. This is CLI startup/error-path smoke, not rendered UI or gaming E2E. Formatting/tidy are not CI gates.
- Appium/self-hosted E2E infrastructure is removed. Before release, manually check profile save/load/duplication and Steam registration in the real QML UI; physical-session controller/audio isolation and teardown with the host helper; and Sunshine/Moonlight pairing, streaming, and cleanup. Tests that inspect host accounts/devices/screens may skip or vary with the environment; green offscreen tests do not prove those hardware paths.
