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
- 🕹️ **Input isolation** — assign gamepads to players from their cards (**Press a button…** or auto-assign), plus keyboards and mice. Each player sees exactly one virtual Xbox 360 pad.
- 👤 **Player isolation** — each player gets a private home with their own save data, settings and Steam library, inside a rootless `bwrap` sandbox with its own session bus (the host network is shared, so LAN and online co-op work). No root helper or extra Linux accounts.
- 🔊 **Audio routing** — per-player PipeWire/PulseAudio sinks.
- 🌙 **Moonlight streaming** — stream a player through Sunshine from a virtual display or a monitor, with per-player pairing.
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
- `couchplay-vdisplay`, which ships with CouchPlay (inside the Flatpak, or from the tarball).

**Steps**

1. On the **Players** page, click **Pair Moonlight client…** on the player and enter the PIN Moonlight shows. Do this once per player.
2. On the player's card, set **Display** to **Virtual display (Moonlight only)**, or pick a monitor and turn on **Stream to Moonlight**.
3. Start the session.
4. In Moonlight, add your PC's address with the port shown on the Players page, and connect.

**Good to know**

- Controllers come from the Moonlight client; CouchPlay assigns them to the streaming player.
- Audio goes to a per-player virtual sink so it plays on the client.
- Each player has their own stream. Keyboard and mouse from the client are not isolated per player.
- A session won't start if a streaming player has no paired client. To start anyway, use `--skip-pairing-check` with `--profile NAME --start`.

### Exiting a Session

Hold **Start + Select for 2 seconds** on any assigned gamepad, or click **Stop Session**. This works with standard gamepads and the Valve Steam Controller (through its gamepad mode), even while Steam is running.

You can also press **Meta + Shift + Escape** (Meta is the Windows/Super key) to stop the session from the keyboard. This is a KDE global shortcut, so it may not work while the keyboard is assigned to a player, because the desktop no longer receives its keys. You can change it in System Settings → Keyboard → Shortcuts → CouchPlay.

### Profiles

Save a session as a profile to reload it later. Deleting a profile asks for confirmation.

### Adding a Profile to Steam

1. Save the profile, then open **Profiles** and click **Add to Steam** on its card.
2. Select a Steam account and close Steam first.
3. Reopen Steam to see the new shortcut.

Native and Flatpak Steam are supported. The shortcut runs `couchplay-gamemode --profile NAME --start --exit-after-session`. Close CouchPlay before launching it; a running instance does not receive the arguments. More in [docs/session-internals.md](docs/session-internals.md#add-to-steam-shortcuts).

### Managing Players

Each player is a private directory (their own home with saves, settings and Steam login), not a Linux account. Use the **Players** page to:

- Add and delete players. Deleting a player removes their saves, logins and Moonlight pairing.
- Pair Moonlight clients per player (see [Streaming a Player to Moonlight](#streaming-a-player-to-moonlight)).

### Directory modes and Player data

Each shared game directory uses one of these modes:

| Mode | What it does |
| --- | --- |
| **Access original folder (read-only)** | Players can read the original folder. Nothing is copied. |
| **Copy files** | Replaces the player's copy on every launch. Player edits are discarded. |
| **Bind** | Shows the original folder inside the player's session only. |
| **Overlay** | Gives the player a private writable layer. The original stays unchanged. |

**Player data** lets you place per-player files for Copy and Overlay directories. Click the button on a player's card to open the staging folder, then put files in the generated subfolder.

Details: [docs/session-internals.md](docs/session-internals.md).

### Settings

Under **Settings**:

- **Hide KDE panels** — auto-hide Plasma panels during sessions.
- **Scaling and filter modes** — per-instance rendering options.
- **Steam and Heroic shortcut sync** — keep launch shortcuts in sync across players.
- **Share game library with players** — players can play the source account's installed Steam games without downloading them again. A new player's Steam installs itself and signs in during their first session, so sharing starts from that player's next session.
- **Steam shortcut source account** — which Steam account supplies shortcuts and game choices. Chosen automatically only if there is exactly one; the Add to Steam account is set separately.
- **Sunshine executable** — override the auto-detected Sunshine path.

## Installation

CouchPlay is a **Flatpak GUI** that launches each player in a rootless sandbox on your host. No root helper or system service is installed; see [Host requirements](#host-requirements) for the few tools it needs.

> **Use the Flatpak for the GUI.** It bundles its own Qt6/KDE Frameworks, so it runs
> identically on every Linux distro. The native `couchplay` binary only works on distros
> whose Qt6 matches the build (Fedora-family) — on Arch or SteamOS it fails to start with
> a copy-relocation error, which is why the Flatpak is the recommended GUI everywhere.

### Quick install (recommended)

One command installs the Flatpak GUI and checks the host requirements:

```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash
```

Beta build (from `develop`):

```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash -s -- --beta
```

> Requires Linux x86_64. The same command works on SteamOS. Beta builds include unreleased features and may be unstable.

### Native binary (tarball, Fedora-family only)

The one-liner defaults to the Flatpak. To install the native `couchplay` GUI binary,
desktop entry and the `couchplay-vdisplay`/`couchplay-padfwd` host tools from the release tarball instead, pass `--tarball`:

```bash
curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash -s -- --tarball
```

The native GUI only runs on Fedora-family distros (where the system Qt6 matches the
build); on Arch/SteamOS it fails to start with a copy-relocation error — use the Flatpak.

### Install the Flatpak manually

1. **Download** the `.flatpak` bundle from the [Releases page](../../releases).
2. **Ensure the runtime is available** (Flathub provides `org.kde.Platform 6.10`):
   ```bash
   flatpak install flathub org.kde.Platform/x86_64/6.10
   ```
3. **Install** the Flatpak:
   ```bash
   flatpak install --user couchplay.flatpak
   ```

### Host requirements

CouchPlay is rootless: it needs no helper service, no root and no system changes. Each player runs as you inside a `bwrap` sandbox, so the host needs:

| Tool | Package |
| --- | --- |
| `bwrap` | `bubblewrap` |
| `dbus-run-session` | `dbus` (`dbus-daemon` on Fedora/Bazzite) |
| `gamescope` | `gamescope` |

Controllers are forwarded through a virtual pad per player, which needs **`/dev/uinput` to be writable by your user**. Steam's udev rules (the `steam-devices` / `steam-support` package) grant this on most distros. `couchplay-padfwd` and `couchplay-vdisplay` ship inside the Flatpak and run on the host from there, so nothing else is installed. The installer only warns if something is missing.

`bwrap` must be allowed to create user namespaces. This is the default on Fedora, Arch and SteamOS-style distros, but Ubuntu 23.10+ blocks it unless `bwrap` has an AppArmor profile. The installer sets that up automatically; see [Troubleshooting](#troubleshooting) if you need to do it by hand. `gamescope` is not packaged in Ubuntu 24.04.

### Upgrading from the helper

Earlier versions used a root helper, temporary Linux accounts, Polkit rules and (on SteamOS) a system extension. To move to the sandbox model, run once from a checkout of this repository:

```bash
sudo ./scripts/migrate-from-helper.sh --flatpak          # dry run: lists the old players and their size
sudo ./scripts/migrate-from-helper.sh --flatpak --yes    # import homes and remove the old helper
# Native tarball install instead of the Flatpak: omit --flatpak.
```

Options: `--user DESKTOP_USER` (defaults to `$SUDO_USER`), `--flatpak` (import into the Flatpak's data directory instead of `~/.local/share/couchplay/players`), `--yes` (actually do it), `--help`. The script copies each old player's home (without `.cache`) into the player store, removes the helper service, Polkit/D-Bus/PipeWire files and the SteamOS system extension, and leaves the old Linux accounts and `/var/lib/couchplay` for you to delete with the `userdel -r` / `rm -rf` commands it prints.

Breaking changes: re-pair your Moonlight clients (pairing state is per player and is recreated), and profiles now store the player under the key `player` (the old `username` key is still read).

### Updating

CouchPlay checks GitHub for a newer **stable** release when it starts (turn this off under **Settings → Software Update**) and shows one notice per launch; **Check Now** checks on demand. The app only informs you.

- **Flatpak:** click **Update Now** in Settings → Software Update, then restart CouchPlay to finish updating. It downloads `couchplay.flatpak` and its `.sha256` for the latest release from GitHub on the host, verifies the checksum (a mismatch aborts without installing anything), installs the bundle into the same Flatpak installation (user or system) as the running app, and removes the temporary files. The one-liner ([Quick install](#quick-install-recommended)) also updates it. If a system-wide installation needs administrator rights, Update Now shows the failure instead of escalating; re-run the one-liner then.
- **Why not `flatpak update`?** CouchPlay is installed from a release bundle, which creates a hidden local remote with no URL. `flatpak update io.github.hikaps.couchplay` therefore always reports "Nothing to update"; use Update Now or the one-liner instead.
- **Native:** stop any running session and re-run the [Quick install](#quick-install-recommended) one-liner with `--tarball`. **Open Releases Page** shows the release notes.

## Troubleshooting

- **Only the first instance starts, with a window-positioning timeout.** This is a KWin window-discovery problem, not a launcher lock. Details are in [docs/session-internals.md](docs/session-internals.md#gamescope-positioning).
- **"CouchPlay needs 'bwrap' on the host" (or dbus-run-session, gamescope).** Install the package named in the message: `bubblewrap`, `dbus` (`dbus-daemon` on Fedora/Bazzite), `gamescope`. See [Host requirements](#host-requirements).
- **Ubuntu: bwrap permission denied (`bwrap: setting up uid map: Permission denied`).** Ubuntu 23.10 and newer (including 24.04) set `kernel.apparmor_restrict_unprivileged_userns=1`, which stops `bwrap` from creating the user namespace a player sandbox needs. The [Quick install](#quick-install-recommended) one-liner detects this, adds a narrow AppArmor profile for `bwrap` only and checks that it works. To do it by hand:

```bash
sudo tee /etc/apparmor.d/bwrap-couchplay >/dev/null <<EOF
abi <abi/4.0>,
include <tunables/global>
profile bwrap-couchplay $(readlink -f "$(command -v bwrap)") flags=(unconfined) {
  userns,
  include if exists <local/bwrap-couchplay>
}
EOF
sudo apparmor_parser -r /etc/apparmor.d/bwrap-couchplay
bwrap --unshare-user --dev-bind / / true && echo works
```

To undo it: `sudo apparmor_parser -R /etc/apparmor.d/bwrap-couchplay && sudo rm /etc/apparmor.d/bwrap-couchplay`. Setting `kernel.apparmor_restrict_unprivileged_userns=0` also works, but it relaxes the restriction for every program.
- **Ubuntu: `gamescope` not found.** It is not in the Ubuntu 24.04 repositories. Install it from a PPA or build it from source, or use a distro that packages it (Fedora, Arch, Bazzite, SteamOS).
- **"Failed to set up controllers".** `couchplay-padfwd` could not open `/dev/uinput`. Make it writable by your user (Steam's udev rules / the `steam-devices` package do this), then log in again.
- **Flatpak closes right after starting and prints `Failed to register name 'com.github.CouchPlay'`.** Seen with the 0.6.0 Flatpak: its sandbox could not own that D-Bus name. Run `flatpak override --user --own-name=com.github.CouchPlay io.github.hikaps.couchplay`, then start it again. A later release fixes this in the Flatpak itself.
- **A game says "This game needs Steam" (or won't start) for one player while another player runs it.** Each player signs in to their own Steam account, so Steam's licensing applies per player. With [Steam Families](https://store.steampowered.com/news/app/593110/view/4605582245626919823), one copy can be played by only one family member at a time. Two players can play the same game at once only if the family owns two copies, e.g. one player owns it and the other borrows the host's copy. CouchPlay cannot work around this.
- **A player's Steam exits with `Couldn't set up Steam data`.** Seen with 0.6.0 on a player who had never run Steam: `~/.steam/steam` in the player's home was a real directory, and Steam's first-run setup needs it to be a link. Check with `ls -ld ~/.local/share/couchplay/players/PLAYER/home/.steam/steam` (under Flatpak the players live in `~/.var/app/io.github.hikaps.couchplay/data/hikaps/CouchPlay/players`). If the line starts with `d`, move it aside (`mv … steam.bak`) and start the session again. Steam then sets itself up, which can take several minutes.
- **Stream won't start or has no picture.** Check Sunshine's log: `~/.local/share/hikaps/CouchPlay/sunshine/<player>/sunshine.log`. Under Flatpak it is in the app's data directory instead (`~/.var/app/io.github.hikaps.couchplay/data/...`; exact path not verified).
- **Stream has no sound.** Run `pactl list short sinks` and look for the player's `couchplay-*` sink.

### More documentation

- [docs/session-internals.md](docs/session-internals.md) — how the sandbox, sessions, directories, Player data and streaming work under the hood.

## Development

### Prerequisites
- CMake 3.20+
- Qt 6.5+ (Core, Quick, Qml, Gui, QuickControls2, Widgets, DBus)
- KDE Frameworks 6 (Kirigami, I18n, Config, CoreAddons, IconThemes, QQC2DesktopStyle, GlobalAccel)
- PipeWire development headers
- Runtime: `bubblewrap`, `dbus` (`dbus-run-session`) and Gamescope; `/dev/uinput` writable by your user

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

## AI Disclosure

This project was developed with assistance from AI tools for code generation, documentation, and debugging.

## Credits

- [DualScope](https://gist.github.com/NaviVani-dev/9a8a704a31313fd5ed5fa68babf7bc3a) by [NaviVani](https://github.com/NaviVani-dev) — initial inspiration for split-screen gaming with Gamescope on Linux.

## License
GPL-3.0-or-later
