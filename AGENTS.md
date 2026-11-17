# Repository Guidelines

## Project Overview

CouchPlay is a C++20/Qt6/KF6/Kirigami application for split-screen gaming on Linux, primarily KDE Plasma/Wayland. It runs per-player Gamescope instances, assigns controllers and audio, supports native/Flatpak launchers, and optionally streams through Sunshine. It is fully rootless: each player is a directory (private HOME) and its game runs as the desktop user inside a `bwrap` sandbox with a private session bus, the shared host network (so LAN and online co-op work), and one virtual Xbox 360 pad.

## Architecture & Data Flow

```text
Kirigami QML → C++ managers → SessionRunner → GamescopeInstance
                                   │               └─ bwrap → dbus-run-session → gamescope → game
                                   ├─ PadForwarder ─ couchplay-padfwd (host, uinput virtual pad per player)
                                   └─ PlayerStore  ─ players/<name>/{home,layers}
```

- `src/main.cpp` initializes Qt/QML, parses profile-launch arguments, and handles single-instance activation through `CommandLineBridge`. `src/qml/Main.qml` constructs managers, injects dependencies, and connects UI notifications.
- `SessionManager` owns editable `SessionProfile`/`InstanceConfig` values. Profiles are KConfig `.conf` files below `QStandardPaths::AppDataLocation/profiles`, not JSON. Persist stable device IDs; runtime input-event numbers are separate.
- `PresetManager` resolves launcher/game selections into `LaunchCommand` (`program`, argv, working directory). Steam/Heroic config managers handle launcher-specific discovery and data; generic applications come from desktop entries/custom presets.
- `SessionRunner` validates and snapshots the starting profile, checks host tools (`bwrap`, `dbus-run-session`, `gamescope`), runs an optional pre-hook, prepares player homes/mounts/virtual pads, and launches instances sequentially. Physical instances use `GamescopeInstance` (sandboxed via `SandboxCommand`) plus KWin window positioning through `WindowManager`.
- Streaming instances get a per-player null audio sink (`StreamAudio`); `StreamManager` generates Sunshine configuration through `SunshineConfig` (persistent per-player state dir) and launches Sunshine as the desktop user on the host via `HostProcess` with KWin capture. Startup timers and bounded restart handling belong to `StreamManager`.
- Stop/startup-failure paths stop pad forwarders and tear down instances, streams, mounts, outputs, and audio before completing the post-hook/final state transition. Preserve this centralized cleanup when changing launch flow.
- `SteamShortcutManager` registers saved profiles as native/Flatpak Game Mode shortcuts. `SteamShortcutsVdf` preserves unrelated binary VDF entries; registration checks paths/concurrent changes, creates a backup, and writes atomically.
- Players are directories under `PlayerStore` (`~/.local/share/couchplay/players/<name>/{home,layers}`; under Flatpak `AppDataLocation/players`). `SessionRunner` starts one `PadForwarder` (`couchplay-padfwd`) per instance, builds per-instance `SandboxMount` lists, and `GamescopeInstance` launches the sandbox through `SandboxCommand`. There is no root helper, D-Bus service, or Polkit.

## Key Directories

| Path | Assistant-relevant purpose |
|---|---|
| `src/core/` | Session lifecycle, launcher/config discovery, device/audio/display management, streaming, Steam registration |
| `src/qml/` | Kirigami pages/components/dialogs; QML module `io.github.hikaps.couchplay` |
| `tools/` | Host-side C programs: `couchplay-padfwd` (per-player virtual Xbox 360 pad via uinput) and `couchplay-vdisplay` (virtual displays); installed to `${CMAKE_INSTALL_LIBEXECDIR}` |
| `tests/` | QtTest executables and explicit CTest target registration |
| `data/` | Desktop file, metainfo, icons |
| `scripts/` | Dependency/bootstrap, installation, Flatpak rebuild (SteamOS), Game Mode launcher, `migrate-from-helper.sh` |
| `.github/workflows/` | Hosted CI, rolling beta, stable release, and Flatpak packaging |

Directory guides exist in `src/core/`, `src/qml/`, and `tests/`. Check their descriptions against current source/CMake: older guides may contain stale persistence or test-build claims.

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
./run-debug.sh --devices

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
- **Composition/ownership:** inject managers (`PlayerStore`, etc.) through existing setters/properties; use QML `required property` for mandatory page dependencies. Parent-owned QObjects cover runner-created instances/processes/`PadForwarder`s; injected dependencies are not runner-owned.
- **State/lifecycle:** `SessionRunner.active` includes startup/hooks/teardown; `running` reflects live instances. Keep these distinct. Use signals, `QTimer`, and `QProcess::finished/errorOccurred` for lifecycle transitions; retain bounded startup/restart behavior and cleanup on failure.
- **Blocking caveat:** `HostProcess` probes, `PadForwarder::start`, and `GamescopeInstance::stop` are synchronous; do not describe the whole launch path as asynchronous. Avoid adding event-loop blocking work. Existing KWin positioning also contains a blocking wait.
- **Errors/logging:** use category-based `qCDebug`/`qCWarning` where available (`Logging.*`), or existing `qDebug`/`qWarning` patterns. User-facing failures emit `errorOccurred(QString)`. Preserve separate startup-failure versus normal-stop signals.
- **QML text/accessibility:** alias Kirigami imports, use `i18nc("@context", "text")` for visible strings, and preserve stable `objectName`/accessible metadata. Required properties and bindings are preferred to hidden global manager lookups.
- **Sandbox boundary:** players are plain directories (`PlayerStore`), never Linux accounts. Games run as the desktop user in `bwrap → dbus-run-session → gamescope`; keep `buildSandboxArgv` ordering invariants (fresh `/dev` before device binds, player home bind before home-subpath mounts, session-bus mask after mounts). Validate player names with `PlayerStore::isValidName` everywhere a name becomes a path, pass argv as structured lists, and never use shell interpolation.

## Important Files

| File | Why to inspect |
|---|---|
| `src/main.cpp`, `src/qml/Main.qml` | CLI/single-instance entry and QML manager composition |
| `src/core/SessionManager.h`, `src/core/LaunchTypes.h` | Profile/instance schema and structured launch values |
| `src/core/SessionRunner.cpp` | Startup, hooks, resource staging, failure handling, teardown |
| `src/core/GamescopeInstance.cpp`, `src/core/WindowManager.cpp` | Gamescope launch arguments and KWin placement |
| `src/core/StreamManager.cpp`, `src/core/SunshineConfig.cpp`, `src/core/StreamAudio.cpp` | Sunshine lifecycle, generated configuration, per-player stream sinks |
| `src/core/PresetManager.cpp` | Launcher/custom-preset discovery and command construction |
| `src/core/SteamShortcutManager.cpp`, `src/core/SteamShortcutsVdf.cpp` | Safe saved-profile registration in Steam |
| `src/core/PlayerStore.cpp`, `src/core/SandboxCommand.cpp`, `src/core/PadForwarder.cpp` | Player directories, bwrap argv builder, per-player virtual pad process wrapper |
| `tools/padfwd/couchplay-padfwd.c`, `tools/padfwd/padmap.c`, `tools/vdisplay/` | Host tools: uinput Xbox 360 pad forwarder and virtual display |
| `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt` | Dependencies, QML packaging, test source/target lists |
| `.github/workflows/ci.yml`, `io.github.hikaps.couchplay.json` | Actual CI gates and Flatpak runtime/build settings |

## Runtime/Tooling Preferences

- Required build stack: CMake ≥ 3.20, C++20 compiler, Qt ≥ 6.5, KF6/ECM ≥ 6.0, Kirigami QML runtime, PipeWire and Wayland development packages. Tests additionally require Qt Test. Fedora bootstrap installs the Qt/KF components listed in CMake.
- This is a Linux host integration app, not a Node/Bun/Python application. Fedora uses `dnf`; builds/tests may run in a dev container. **Runtime host requirements:** `bubblewrap` (`bwrap`), `dbus` (`dbus-run-session`), `gamescope`, and a user-writable `/dev/uinput`. Real Gamescope/input/audio/KWin checks require the host display and these tools; offscreen CI cannot substitute for them.
- Native binaries need compatible host Qt/KF libraries. Prefer the Flatpak GUI on Arch/SteamOS rather than a Fedora-built tarball. Game Mode launch requires `kwin_wayland` in its runtime. The Flatpak manifest uses KDE Platform/SDK 6.10.
- Host tools `couchplay-vdisplay` and `couchplay-padfwd` (built from `tools/`) install to `${CMAKE_INSTALL_LIBEXECDIR}`; under Flatpak they run on the host straight from the Flatpak app path (`HostProcess::hostLibexecPath`), so nothing is installed on the host. `run-debug.sh` consumes its first argument as the logging selector and forwards subsequent arguments to CouchPlay.
- App/Flatpak/QML ID is `io.github.hikaps.couchplay`.
- PRs target `develop`; stable tags/releases belong to `main`. `beta.yml` publishes from `develop`. Do not mistake reduced beta/release test selections for the full develop CI gate.

## Testing & QA

- QtTest executables are registered with CTest in `tests/CMakeLists.txt`. Tests compile shared manager source lists into each executable, not a production library and not direct `.cpp` includes. Register new targets through the existing `add_couchplay_test` helper.
- Follow handwritten test doubles (`FakeGamescopeInstance` via `SessionRunner::setInstanceFactory`, `PadForwarder::setProgramOverrideForTesting` with a shell-script stub), `QTemporaryDir`, scoped environment restoration, and Qt test paths. Some tests use `#define private public`; reuse existing seams rather than adding a mocking framework. Use `QSignalSpy` and bounded `QTRY_*` assertions for async transitions.
- Cover changed observable behavior: profile persistence/migration, launch validation, lifecycle/failure cleanup, config parsing, sandbox argv ordering (`SandboxCommand`), and player-name/path boundaries (`PlayerStore`). Steam tests use fabricated account/VDF trees under a fake player home.
- `.github/workflows/ci.yml` runs the full suite on a hosted runner inside Fedora 41, then launches the real app offscreen/software with isolated HOME/XDG paths and timeouts: `--help` succeeds; `--start` without a profile exits 2; an absent profile exits 1. This is CLI startup/error-path smoke, not rendered UI or gaming E2E. Formatting/tidy are not CI gates.
- Appium/self-hosted E2E infrastructure is removed. Before release, manually check profile save/load/duplication and Steam registration in the real QML UI; physical-session controller/audio isolation (one virtual pad per player, sandbox `/dev`) and teardown with `couchplay-padfwd`/`bwrap`; and Sunshine/Moonlight pairing, streaming, and cleanup. Tests that inspect host devices/screens may skip or vary with the environment; green offscreen tests do not prove those hardware paths.
