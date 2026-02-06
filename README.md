# CouchPlay

Split-screen gaming manager for Linux, designed for KDE Plasma and Gamescope. CouchPlay enables multi-seat gaming sessions on a single PC by managing input device assignment, multiple Gamescope instances, and audio routing.

## Features

- 🎮 **Input Isolation**: Assign specific gamepads/keyboards to specific player instances.
- 🖥️ **Multi-Instance**: Run multiple games simultaneously using Gamescope nested compositors.
- 🔊 **Audio Routing**: Route game audio to specific outputs (via PipeWire).
- 👤 **User Management**: Automatically manages temporary user accounts for isolated save data.
- 🐧 **Atomic-Ready**: Designed for immutable distributions like Bazzite and Fedora Silverblue.

## Installation (Bazzite / Fedora Atomic)

CouchPlay uses a privileged helper to manage devices and users.

1. **Download** the latest release tarball from the [Releases page](../../releases).
2. **Extract** the archive:
   ```bash
   tar -xzf couchplay-*-linux.tar.gz
   cd couchplay-*
   ```
3. **Install** the helper service (requires sudo):
   ```bash
   sudo ./install-helper.sh install
   ```
4. **Run** the application:
   ```bash
   ./bin/couchplay
   ```

### Uninstallation
```bash
sudo ./install-helper.sh uninstall
```

## Development

### Prerequisites
- CMake 3.20+
- Qt 6.5+
- KDE Frameworks 6 (Kirigami, I18n, Config, CoreAddons)
- Gamescope
- PipeWire (devel headers)
- Polkit (devel headers)

### Building
```bash
cmake -B build
cmake --build build
```

### Running Tests
```bash
ctest --test-dir build --output-on-failure
```

## License
GPL-3.0-or-later
