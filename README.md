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
- ⏹️ **Controller exit chord** — hold **Start + Select** on any assigned gamepad for 2 seconds to stop the entire split-screen session without touching a keyboard.

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

### Exiting a Session

While a session is running you can stop it directly from the couch — no keyboard needed:

- **Hold Start + Select for 2 seconds** on any assigned gamepad to stop the entire split-screen session.

This works with standard gamepads (Xbox, PlayStation, generic HID) and with the **Valve Steam Controller** — even when Steam is active and holding the evdev node, since CouchPlay reads Steam Controller input directly from the raw HID (`/dev/hidrawN`) endpoint.

Alternatively, use the **Stop Session** button in the CouchPlay UI.

### Profiles

Save a session configuration as a profile to reload it later without reconfiguring everything. Profiles are stored as JSON files in `~/.local/share/couchplay/profiles/` and can be loaded from the **Profiles** page.

### Managing Users

CouchPlay creates temporary Linux user accounts so each player gets isolated save data and settings. Open the **Users** page to view and manage these accounts. The helper service handles the privileged operations behind the scenes.

### Settings

The **Settings** page offers additional configuration:
- **Hide KDE panels** — auto-hide Plasma panels during gaming sessions.
- **Scaling and filter modes** — per-instance rendering options.
- **Steam and Heroic shortcut sync** — keep launch shortcuts in sync across users.

## Installation

CouchPlay has two parts: a **Flatpak GUI** (the app you interact with) and a small
**privileged helper** (a root systemd/D-Bus service that manages input devices, users,
and audio). Install the Flatpak for the GUI, then install the helper.

> **Use the Flatpak for the GUI.** It bundles its own Qt6/KDE Frameworks, so it runs
> identically on every Linux distro. The native `couchplay` binary only works on distros
> whose Qt6 matches the build (Fedora-family) — on Arch or SteamOS it fails to start with
> a copy-relocation error, which is why the Flatpak is the recommended GUI everywhere.

### 1. Install the Flatpak (GUI)

1. **Download** the `.flatpak` bundle from the [Releases page](../../releases).
2. **Ensure the runtime is available** (Flathub provides `org.kde.Platform 6.10`):
   ```bash
   flatpak install flathub org.kde.Platform/x86_64/6.10
   ```
3. **Install** the Flatpak:
   ```bash
   flatpak install --user couchplay.flatpak
   ```

### 2. Install the helper

The helper is required. Its runtime libraries are bundled, so it starts without any
system Qt6/KF6 — the same helper works on every distro. Install it from inside the
Flatpak, or standalone with `install.sh`.

**From the Flatpak** (the helper ships inside it):
```bash
flatpak run --command=bash io.github.hikaps.couchplay -c "/app/share/couchplay/install-helper.sh export"
sudo ~/.var/app/io.github.hikaps.couchplay/data/couchplay/install-helper.sh install
```
The `export` step stages the helper in the Flatpak's persisted data directory
(`~/.var/app/io.github.hikaps.couchplay/data/couchplay`), visible on your host at the
same path; the second command installs it system-wide from there.

**Standalone (curl one-liner):**
```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash
```
Beta (from `develop`):
```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash -s -- --beta
```
> Requires Linux x86_64 and sudo. Beta builds include unreleased features and may be unstable.
> `install.sh` also installs a native `couchplay` GUI binary and desktop entry. The
> helper works on every distro; the native GUI only runs on Fedora-family distros —
> on Arch/SteamOS, keep using the Flatpak above for the GUI.

**Uninstall the helper:**
```bash
# Installed via the Flatpak export:
sudo ~/.var/app/io.github.hikaps.couchplay/data/couchplay/install-helper.sh uninstall
# Installed standalone — re-download the tarball, then from the extracted directory:
sudo ./install-helper.sh uninstall
```

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
Run the app you just built (only on a distro with matching Qt6, e.g. Fedora — otherwise use the Flatpak):
```bash
./build/bin/couchplay
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
