<p align="center">
  <img src="assets/icon.png" alt="CouchPlay" width="128"/>
</p>

<h1 align="center">CouchPlay</h1>

<p align="center">
  <strong>Couch co-op for any game on Linux: split-screen it, or stream players to Moonlight.</strong><br/>
  CouchPlay launches multiple Steam or Heroic instances side by side through Gamescope — no native split-screen support needed — and can stream any player to a Moonlight client through Sunshine. Each player gets their own isolated input, save data, and audio. Works on KDE Plasma (Wayland), including immutable distros like Bazzite.
</p>

<p align="center">
  <a href="https://github.com/hikaps/couchplay/actions/workflows/ci.yml"><img src="https://github.com/hikaps/couchplay/actions/workflows/ci.yml/badge.svg?branch=develop" alt="CI"/></a>
  <a href="https://github.com/hikaps/couchplay/actions/workflows/flatpak.yml"><img src="https://github.com/hikaps/couchplay/actions/workflows/flatpak.yml/badge.svg?branch=develop" alt="Flatpak Build"/></a>
  <a href="https://github.com/hikaps/couchplay/releases/latest"><img src="https://img.shields.io/github/v/release/hikaps/couchplay?label=release" alt="Latest release"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue" alt="License: GPL-3.0-or-later"/></a>
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
- 🕹️ **Input isolation** — assign gamepads to players from their cards (**Press a button…** or auto-assign), plus keyboards and mice. Steam Input virtual gamepads are detected and isolated automatically.
- 👤 **User isolation** — each player gets a temporary Linux account with their own save data, settings, and Steam library.
- 🔊 **Audio routing** — cross-user audio via PipeWire PulseAudio TCP listener.
- 🌙 **Moonlight streaming** — stream a player through Sunshine from a virtual display or a monitor, with per-user pairing.
- 🧭 **Per-player display placement** — put each player on the main screen, a specific monitor, or a virtual display.
- ⚡ **Sequential launching** — instances start one at a time to avoid GPU contention.
- 🖼️ **Borderless windows** — optional borderless mode for cleaner multi-window setups.
- 🐧 **Runs on many distros** — the Flatpak GUI works on any distro with KDE Plasma on Wayland, including immutable ones like Bazzite. Developed and tested on Bazzite; the installer also supports SteamOS, and CI builds on Fedora. Arch-based distros such as CachyOS are expected to work via the Flatpak but haven't been tested.
- ⏹️ **Controller exit chord** — hold **Start + Select** on any assigned gamepad for 2 seconds to stop the entire split-screen session without touching a keyboard.

## Usage

### Creating a Session

1. Open **New Session** from the sidebar.
2. Pick a layout: **Side by Side**, **Top and Bottom**, **Multi-Monitor** (one display per player), or **Grid** (3+ players, as 3×1 or 2×2).
3. For each player, pick a launcher (Steam, Steam Big Picture or Heroic), then resolution, refresh rate, scaling and filter.
4. Choose the player's **Display**: **Main screen** (the default, placed by the layout), a specific monitor, or **Virtual display (Moonlight only)**.
5. Click **Start Session**.

### Assigning Controllers

Connect your gamepads, then either:

- Click **Auto-Assign Controllers**, or
- Click **Press a button…** on a player's card and press any button on the controller you want.

Keyboards, mice and drag-and-drop assignment are under **Other devices…**. Each controller is locked to its player so inputs don't leak.

### Streaming a Player to Moonlight

A player can be streamed to a [Moonlight](https://moonlight-stream.org/) client instead of (or as well as) a local screen.

> Streaming is new in 0.6 and has had limited hardware testing; please report problems.

**Requirements**

- KDE Plasma with KWin on Wayland.
- [Sunshine](https://github.com/LizardByte/Sunshine) installed on the host as a regular binary. CouchPlay looks for it at `/usr/bin`, `/usr/local/bin`, `/home/linuxbrew/.linuxbrew/bin` and `~/.local/bin`. You can set the path in **Settings**. From the Flatpak it is run on the host through `flatpak-spawn --host`.
- `couchplay-vdisplay`, which is installed with the helper.

**Steps**

1. On the **Users** page, click **Pair Moonlight client…** on the gaming user and enter the PIN Moonlight shows. Do this once per user.
2. On the player's card, set **Display** to **Virtual display (Moonlight only)**, or pick a monitor and turn on **Stream to Moonlight**.
3. Start the session.
4. In Moonlight, add your PC's address with the port shown on the Users page, and connect.

**Good to know**

- Controllers come from the Moonlight client; CouchPlay assigns them to the streaming player.
- Audio goes to a per-player virtual sink so it plays on the client.
- Each player has their own stream. Keyboard and mouse from the client are not isolated per player.
- A session won't start if a streaming player has no paired client. To start anyway, use `--skip-pairing-check` with `--profile NAME --start`.

### Exiting a Session

Hold **Start + Select for 2 seconds** on any assigned gamepad, or click **Stop Session**. This works with standard gamepads and the Valve Steam Controller, even while Steam is running.

You can also press **Meta + Shift + Escape** (Meta is the Windows/Super key) to stop the session from the keyboard. This is a KDE global shortcut, so it may not work while the keyboard is assigned to a player, because the desktop no longer receives its keys. You can change it in System Settings → Keyboard → Shortcuts → CouchPlay.

### Profiles

Save a session as a profile to reload it later. Deleting a profile asks for confirmation.

### Adding a Profile to Steam

1. Save the profile, then open **Profiles** and click **Add to Steam** on its card.
2. Select a Steam account and close Steam first.
3. Reopen Steam to see the new shortcut.

Native and Flatpak Steam are supported. The shortcut runs `couchplay-gamemode --profile NAME --start --exit-after-session`. Close CouchPlay before launching it; a running instance does not receive the arguments. More in [docs/session-internals.md](docs/session-internals.md#add-to-steam-shortcuts).

### Managing Users

CouchPlay creates temporary Linux accounts so each player gets their own saves and settings. Use the **Users** page to:

- Create and delete gaming users. This needs the [helper](#2-install-the-helper); the buttons are disabled without it.
- Pair Moonlight clients per user (see [Streaming a Player to Moonlight](#streaming-a-player-to-moonlight)).

Account changes ask for your password through a Polkit prompt. Session actions use a separate prompt, and approval may be remembered for a while.

### Directory modes and Player data

Each shared game directory uses one of these modes:

| Mode | What it does |
| --- | --- |
| **Access original folder (ACL)** | Players can read the original folder. Nothing is copied. |
| **Copy files** | Replaces the player's copy on every launch. Player edits are discarded. |
| **Bind** | Shows the original folder inside the player's session only. |
| **Overlay** | Gives the player a private writable layer. The original stays unchanged. |

**Player data** lets you place per-player files for Copy and Overlay directories. Click the button on a player's card to open the staging folder, then put files in the generated subfolder.

Details: [docs/session-internals.md](docs/session-internals.md).

### Settings

Under **Settings**:

- **Hide KDE panels** — auto-hide Plasma panels during sessions.
- **Scaling and filter modes** — per-instance rendering options.
- **Steam and Heroic shortcut sync** — keep launch shortcuts in sync across users.
- **Share game library with players** — players can play the source account's installed Steam games without downloading them again. A new player's Steam installs itself and signs in during their first session, so sharing starts from that player's next session.
- **Steam shortcut source account** — which Steam account supplies shortcuts and game choices. Chosen automatically only if there is exactly one; the Add to Steam account is set separately.
- **Sunshine executable** — override the auto-detected Sunshine path.

## Installation

CouchPlay has two parts: a **Flatpak GUI** (the app you interact with) and a small
**privileged helper** (a root systemd/D-Bus service that manages input devices, users,
and audio). Install the Flatpak for the GUI, then install the helper.

> **Use the Flatpak for the GUI.** It bundles its own Qt6/KDE Frameworks, so it runs
> identically on every Linux distro. The native `couchplay` binary only works on distros
> whose Qt6 matches the build (Fedora-family) — on Arch or SteamOS it fails to start with
> a copy-relocation error, which is why the Flatpak is the recommended GUI everywhere.

### Quick install (recommended)

One command installs the Flatpak GUI and the privileged helper:

```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash
```

Beta build (from `develop`):

```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash -s -- --beta
```

> Requires Linux x86_64 and sudo. On SteamOS the installer also deploys a system extension
> for the privileged helper. Beta builds include unreleased features and may be unstable.

### Native binary (tarball, Fedora-family only)

The one-liner defaults to the Flatpak. To install the native `couchplay` GUI binary +
desktop entry and the privileged helper from the release tarball instead, pass `--tarball`:

```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash -s -- --tarball
```

The native GUI only runs on Fedora-family distros (where the system Qt6 matches the
build); on Arch/SteamOS it fails to start with a copy-relocation error — use the Flatpak.

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

**One-liner:** see **Quick install** at the top of this section - it installs the Flatpak GUI and the helper together.

**Uninstall the helper:**
```bash
# Installed via the Flatpak export:
sudo ~/.var/app/io.github.hikaps.couchplay/data/couchplay/install-helper.sh uninstall
# Installed standalone — re-download the tarball, then from the extracted directory:
sudo ./install-helper.sh uninstall
```

### Updating

CouchPlay checks GitHub for a newer **stable** release when it starts (turn this off under **Settings → Software Update**) and shows one notice per launch; **Check Now** checks on demand. The app only informs you: it never updates the privileged helper or the SteamOS system extension itself.

- **Flatpak:** click **Update Now** in Settings → Software Update, then restart CouchPlay to finish updating. It downloads `couchplay.flatpak` and its `.sha256` for the latest release from GitHub on the host, verifies the checksum (a mismatch aborts without installing anything), installs the bundle into the same Flatpak installation (user or system) as the running app, and removes the temporary files. The one-liner ([Quick install](#quick-install-recommended)) also updates it. If a system-wide installation needs administrator rights, Update Now shows the failure instead of escalating; re-run the one-liner then.
- **Why not `flatpak update`?** CouchPlay is installed from a release bundle, which creates a hidden local remote with no URL. `flatpak update io.github.hikaps.couchplay` therefore always reports "Nothing to update"; use Update Now or the one-liner instead.
- **Native or SteamOS:** stop any running session and re-run the [Quick install](#quick-install-recommended) one-liner (add `--tarball` to keep the native binary). On SteamOS this also refreshes the system extension. **Open Releases Page** shows the release notes.
- **Helper:** Update Now does not touch it. The GUI keeps working with an older helper until a release needs a newer one. Settings → Privileged Helper shows the installed helper version and warns when it is too old for the GUI; reinstall it then (see [Install the helper](#2-install-the-helper)).

## Troubleshooting

- **Only the first instance starts, with a window-positioning timeout.** This is a KWin window-discovery problem, not a launcher lock. Details are in [docs/session-internals.md](docs/session-internals.md#gamescope-positioning).
- **Errors after updating.** Install the GUI and the helper together, and stop all sessions first. Upgrading to 0.6 changes the helper's D-Bus API and removes the `manage-virtual-display` and `manage-audio-sink` Polkit actions, so reinstall the helper (see [Installation](#2-install-the-helper)).
- **Password prompt hidden behind the game windows, or a session that won't stop.** Fixed in 0.6.1; reinstall the helper. Session operations no longer ask for a password on an active local session (see [session-internals](docs/session-internals.md#helper-authorization) for the trade-off). Creating and deleting players still asks.
- **Flatpak closes right after starting and prints `Failed to register name 'com.github.CouchPlay'`.** Seen with the 0.6.0 Flatpak: its sandbox could not own that D-Bus name. Run `flatpak override --user --own-name=com.github.CouchPlay io.github.hikaps.couchplay`, then start it again. A later release fixes this in the Flatpak itself.
- **A game says "This game needs Steam" (or won't start) for one player while another player runs it.** Each player signs in to their own Steam account, so Steam's licensing applies per player. With [Steam Families](https://store.steampowered.com/news/app/593110/view/4605582245626919823), one copy can be played by only one family member at a time. Two players can play the same game at once only if the family owns two copies, e.g. one player owns it and the other borrows the host's copy. CouchPlay cannot work around this.
- **A player's Steam exits with `Couldn't set up Steam data`.** Seen with 0.6.0 on a player who had never run Steam: CouchPlay left `~/.steam/steam` in the player's home as a real directory, and Steam's first-run setup needs it to be a link. Check with `sudo ls -ld /home/PLAYER/.steam/steam` (on Bazzite and other atomic distros the home is under `/var/home`). If the line starts with `d`, move it aside with `sudo mv /home/PLAYER/.steam/steam /home/PLAYER/.steam/steam.bak` and start the session again. Steam then sets itself up, which can take several minutes. Later releases no longer create the directory, but do not remove one that already exists.
- **Stream won't start or has no picture.** Check Sunshine's log: `~/.local/share/hikaps/CouchPlay/sunshine/<user>/sunshine.log`. Under Flatpak it is in the app's data directory instead (`~/.var/app/io.github.hikaps.couchplay/data/...`; exact path not verified).
- **Stream has no sound.** Run `pactl list short sinks` and look for the player's `couchplay-*` sink.

### More documentation

- [docs/session-internals.md](docs/session-internals.md) — how sessions, directories, Player data and streaming work under the hood.
- [docs/helper-internals.md](docs/helper-internals.md) — the privileged helper, mounts, ACLs, recovery and Flatpak path mapping.

## Development

### Prerequisites
- CMake 3.20+
- Qt 6.5+ (Core, Quick, Qml, Gui, QuickControls2, Widgets, DBus)
- KDE Frameworks 6 (Kirigami, I18n, Config, CoreAddons, IconThemes, QQC2DesktopStyle, GlobalAccel)
- PolkitQt6-1 (required)
- PipeWire and libacl development headers
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
(
    runtime=$(mktemp -d)
    trap 'rm -rf -- "$runtime"' EXIT
    chmod 700 "$runtime"
    export XDG_RUNTIME_DIR="$runtime" QT_QPA_PLATFORM=offscreen
    dbus-run-session -- ctest --test-dir build --output-on-failure
)
```

The optional `COUCHPLAY_PRIVILEGED_TESTS=ON` namespace gate runs only in its disposable
privileged Linux container and requires fanotify permission events. Preparation races,
cancellation, and restart checks hold fixture-file opens explicitly rather than relying
on copy size or scheduling speed.

## AI Disclosure

This project was developed with assistance from AI tools for code generation, documentation, and debugging.

## Credits

- [DualScope](https://gist.github.com/NaviVani-dev/9a8a704a31313fd5ed5fa68babf7bc3a) by [NaviVani](https://github.com/NaviVani-dev) — initial inspiration for split-screen gaming with Gamescope on Linux.

## License
GPL-3.0-or-later
