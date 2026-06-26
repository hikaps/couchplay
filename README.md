<p align="center">
  <img src="assets/icon.png" alt="CouchPlay" width="128"/>
</p>

<h1 align="center">CouchPlay</h1>

<p align="center">
  <strong>Split-screen any game on Linux.</strong><br/>
  CouchPlay launches multiple Steam or Heroic instances side by side through Gamescope — no native split-screen support needed. Each player gets their own isolated input, save data, and audio. Works on KDE Plasma, designed for immutable distros like Bazzite and Fedora Silverblue.
</p>

## Screenshots

<p align="center">
  <img src="assets/couchplay-main-cropped.png" alt="CouchPlay Home" width="80%"/>
</p>

<p align="center">
  <img src="assets/couchplay-session-cropped.png" alt="CouchPlay Session Configuration" width="80%"/>
</p>

<p align="center">
  <img src="assets/couchplay-split-screen.png" alt="CouchPlay Split-Screen Gaming" width="80%"/>
</p>

## Features

- 🎮 **Split-screen any game** — launches multiple independent Steam or Heroic instances through Gamescope. No native split-screen support required.
- 🖥️ **Flexible layouts** — side by side, top and bottom, multi-monitor, or grid (3+ players with 3×1 and 2×2 sub-layouts).
- 🕹️ **Input isolation** — assign gamepads, keyboards, and mice to specific player instances. Steam Input virtual gamepads are detected and isolated automatically.
- 👤 **User isolation** — each player gets a temporary Linux account with their own save data, settings, and Steam library.
- 🔊 **Audio routing** — cross-user audio via PipeWire PulseAudio TCP listener.
- 🛡️ **Overlay configs** — per-user config file overrides via glob patterns.
- ⚡ **Sequential launching** — instances start one at a time to avoid GPU contention.
- 🖼️ **Borderless windows** — optional borderless mode for cleaner multi-window setups.
- 🐧 **Atomic-ready** — designed for immutable distributions like Bazzite and Fedora Silverblue. Available as a Flatpak.

## Usage

### Creating a Session

1. Open the **New Session** page from the sidebar.
2. Choose a screen layout that fits your setup:
   - **Side by Side** — splits the screen horizontally
   - **Top and Bottom** — splits the screen vertically
   - **Multi-Monitor** — dedicates one display per player
   - **Grid** — supports 3+ players with sub-layouts (3×1 row or 2×2 with gap)
3. Configure each player instance: pick a launcher (Steam, Steam Big Picture, or Heroic), then set resolution, refresh rate, scaling, and filter options.
4. Click **Start Session** to launch all instances through Gamescope.

### Assigning Controllers

Connect your gamepads, then use the **Auto-Assign Controllers** button to let CouchPlay distribute them across player instances. You can also drag and drop devices for manual assignment. Each controller gets locked to its assigned instance so inputs don't leak between players.

### Profiles

Save a session configuration as a profile to reload it later without reconfiguring everything. Profiles are stored as JSON files in `~/.local/share/couchplay/profiles/` and can be loaded from the **Profiles** page.

### Managing Users

CouchPlay creates temporary Linux user accounts so each player gets isolated save data and settings. Open the **Users** page to view and manage these accounts. The helper service handles the privileged operations behind the scenes.

### Settings

The **Settings** page offers additional configuration:
- **Hide KDE panels** — auto-hide Plasma panels during gaming sessions.
- **Scaling and filter modes** — per-instance rendering options.
- **Steam and Heroic shortcut sync** — keep launch shortcuts in sync across users.

## Quick Install (Linux x86_64)

Install the latest release with a single command:

```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash
```

> **Requirements**: Linux x86_64, root privileges (sudo). This downloads and installs the latest release from GitHub, including the privileged helper service.

### Beta (from develop)

Install the latest build from the `develop` branch:

```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash -s -- --beta
```

> **Warning**: Beta builds include unreleased features and may be unstable. For production use, install the stable release above.

## Installation (Bazzite / Fedora Atomic)

CouchPlay uses a privileged helper to manage devices and users.

1. **Download** the latest release tarball from the [Releases page](../../releases).
2. **Extract** the archive:
   ```bash
   tar -xJf couchplay-x86_64.tar.xz
   cd couchplay-x86_64
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

## Flatpak Installation

1. **Download** the `.flatpak` bundle from the [Releases page](../../releases).
2. **Ensure the runtime is available** (Flathub provides org.kde.Platform 6.10):
   ```bash
   flatpak install flathub org.kde.Platform/x86_64/6.10
   ```
3. **Install** the Flatpak:
   ```bash
   flatpak install --user couchplay.flatpak
   ```
4. **Install the helper service** (required for device management):
   ```bash
   flatpak run --command=bash io.github.hikaps.couchplay -c "/app/share/couchplay/install-helper.sh export"
   sudo ~/.local/share/couchplay/install-helper.sh install
   ```

   The `export` step copies the helper and install script to `~/.local/share/couchplay`
   on your host (this requires the `--filesystem=home` permission, included in current
   builds). If you are on an **older Flatpak build** where `export` produced no files in
   `~/.local/share/couchplay`, the files were written to the Flatpak sandbox home instead —
   install from there:
   ```bash
   sudo ~/.var/app/io.github.hikaps.couchplay/.local/share/couchplay/install-helper.sh install
   ```

> **Note**: The tarball installation method above is also available if you prefer it or your distribution doesn't support Flatpak.

## Development

### Prerequisites
- CMake 3.20+
- Qt 6.5+ (Core, Quick, Qml, Gui, QuickControls2, Widgets, DBus)
- KDE Frameworks 6 (Kirigami, I18n, Config, CoreAddons, IconThemes, QQC2DesktopStyle, GlobalAccel)
- PolkitQt6-1 (optional — required for user management authorization)
- PipeWire (devel headers)
- Gamescope (runtime dependency)

### Building
```bash
cmake -B build
cmake --build build
```

### Running Tests
```bash
ctest --test-dir build --output-on-failure
```

## AI Disclosure

This project was developed with assistance from AI tools for code generation, documentation, and debugging.

## Credits

- [DualScope](https://gist.github.com/NaviVani-dev/9a8a704a31313fd5ed5fa68babf7bc3a) by [NaviVani](https://github.com/NaviVani-dev) — initial inspiration for split-screen gaming with Gamescope on Linux.

## License
GPL-3.0-or-later
