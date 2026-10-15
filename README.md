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

Save a session configuration as a profile to reload it later without reconfiguring everything. On the **Profiles** page, card actions wrap on narrow windows so **Delete** remains accessible; deleting a profile requires confirmation.

### Adding a Profile to Steam

Save the session profile, then open **Profiles** in Desktop Mode and choose **Add to Steam** on its card. Select a Steam account and close Steam manually before adding the shortcut. Reopen Steam manually afterward to refresh its shortcut list; reopening it does not switch accounts.

Account discovery supports native Steam and Flatpak Steam, including its `~/.var/app/com.valvesoftware.Steam/data/Steam` directory. Shortcut files that exceed the supported byte or record limits are left unchanged.
Symlinked shortcut and backup files, including dangling links, are rejected without writing to their targets.

The shortcut launches the installed `couchplay-gamemode` command with `--profile NAME --start --exit-after-session`, starting the saved profile and its configured games. If CouchPlay is already open, close it first: the current singleton does not forward shortcut arguments to an existing instance.

### Managing Users

CouchPlay creates temporary Linux user accounts so each player gets isolated save data and settings. Open the **Users** page to view and manage these accounts. The helper service handles the privileged operations behind the scenes.
Managed users reload automatically when the helper is attached or becomes available; opening CouchPlay does not require a manual user refresh.

Normal session operations use the shared Polkit `manage-session` action (`auth_admin_keep`). Polkit may cache approval for a bounded period, but it is not permanent or guaranteed to last through the CouchPlay session. Account creation, deletion, and explicit linger changes keep their separate Polkit actions.

On immutable hosts such as Bazzite, Heroic configuration and shortcut copies resolve the system `/home` alias before secure copying. Symlinked source files and directories inside the caller's home remain rejected.
The Flatpak shares `~/.local/share/applications` read/write so newly generated Heroic desktop shortcuts are visible to the host helper; it does not require access to the entire home directory.
Launcher availability is checked on the host, not against executable names inside the GUI sandbox.
Heroic sessions resolve current registered game roots and executable-parent directories, granting bounded directory traversal without copying account credentials or per-player game settings. On atomic hosts, the helper admits the canonical /var/mnt storage alias only when the system /mnt path proves equivalent.
Native application selection adds bounded executable and working directories as read-only ACL shares; session startup applies the selected application's current shared-directory settings to each player. Host discovery supports Flatpak installations. CouchPlay does not automatically share whole home folders, collection roots, credentials/configuration trees, or Flatpak runtime files. Sensitive home trees and canonical targets are excluded from recursive sharing; application directories under /opt and /srv are supported without granting access to those roots.
Player settings are shown directly in New Session, without a separate Advanced section. Private temporary directories gives that player's Gamescope/launcher/game processes separate /tmp and /var/tmp storage. The option defaults off, is saved per player in the profile, and removes temporary files when the instance stops. It does not affect shared-directory settings or auxiliary Sunshine configuration. Install the matching GUI and host helper together; the launch D-Bus contract includes this option.
Application directory settings belong to the application, not a saved player profile. Editing them applies to all profiles on their next launch; a running session keeps the values captured when it started.

Directory modes differ: **Access original folder (ACL)** grants read/traverse permission at the source path; it does not project files into player HOME. **Copy files** replaces the entire mapped player directory on every launch, discarding player edits and player-only files; Player data staging is merged before atomic publication and wins conflicts. A copy is published atomically; failure or cancellation before publication retains the previous directory. **Bind** exposes the source only inside the player's unit namespace and preserves source permissions. **Overlay** provides a persistent private writable layer inside that namespace while leaving the source unchanged. Filesystem/kernel ownership-mapping support is required for Overlay; unsupported sources produce an explicit error recommending Copy files.
Launch requests support desktop integration (`XDG_CURRENT_DESKTOP`) and per-instance audio routing (`PULSE_SINK`); identity, runtime-directory, and audio-server environment values remain helper-owned.

**Player data** supports configured Copy/Overlay directories that receive staged files. ACL/bind shares and Steam library sharing do not qualify. The button opens a staging folder for the selected launcher and player, including under Flatpak. Each eligible directory has a generated subfolder: place files there with their relative paths preserved. Session startup applies staged files as part of directory preparation. Loose files at the staging root are ignored. This folder is not the player's Linux home or live /tmp. Automation hooks run once before/after the whole session, not before each instance.
The former Config Overrides/file-pattern feature has been removed. Its old profile settings are ignored and discarded when profiles are saved; existing override files on disk are left untouched. Use configured Copy/Overlay directories and Player data for per-player configurations.
Gamescope positioning uses host-visible scripts and waits for KWin to confirm the requested frame geometry before reporting success.

### Settings

The **Settings** page offers additional configuration:
- **Hide KDE panels** — auto-hide Plasma panels during gaming sessions.
- **Scaling and filter modes** — per-instance rendering options.
- **Steam and Heroic shortcut sync** — keep launch shortcuts in sync across users.
- **Steam shortcut source account** — select which native/Flatpak Steam account supplies shortcut counts, game choices, and shortcuts synced to players. CouchPlay auto-selects only when it finds one valid account; choose explicitly when there are multiple. The Add to Steam destination account is selected separately.

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

## Troubleshooting

If only the first instance starts and CouchPlay reports a window-positioning timeout,
check KWin window discovery rather than launcher PID locks. Session startup matches each
window to the host Gamescope PID; Wayland Gamescope windows can have an empty application ID.
Gamescope can replace its initial window after a launcher loads. A PID-scoped KWin
listener positions replacement windows in the same player slot until the session stops.

Flatpak directory selections are resolved from document-portal exports to host paths
before being saved as shared directories. The GUI and helper share one mapping
rule: strict descendants of either the account's declared home or its canonical
path are home-relative. External paths remain external; traversal and whole-home
sharing without an explicit target alias are rejected. This applies to any
symlinked home, including `/home` → `/var/home` on Atomic desktops. Both home
paths are resolved by the host helper, not inside the Flatpak sandbox. Update
the GUI and helper together when installing this fix. Secure path walks require
only search permission on ancestor directories and retain read checks on the
selected directory. Explicitly projected sources, including symlinked Steam libraries,
are resolved on the host and revalidated before no-follow pinning. Implicit target
routing and staging directories retain strict no-follow path walks.

Bind and Overlay views exist in each instance's private mount namespace, not in the
host file manager or helper service. Startup reports preparation errors before Gamescope
window discovery. Stop all sessions before updating the matching GUI and native helper.
Directory sources are carried by the preparation plan, independently of working-directory admission.
Detached source mounts are made private before attachment, so shared host mounts
cannot propagate a player Overlay back onto the original source directory.
If an older build leaked an Overlay, inspect the mount before changing ownership:
the player-owned view may be hiding an unchanged host directory. After uncovering
the host directory, restart the helper so its private namespace does not retain
the old leaked view.
Gamescope is projected read-only with `nosuid` inside each instance so distro file
capabilities do not prevent an unprivileged launch; the host binary is unchanged.

Player staging directories are resolved to canonical paths before helper handoff,
including linked application-data storage. The helper still requires caller-owned
home storage and no-follow traversal of the resolved directories and staged files.

Recursive ACL preparation runs in cancellable instance startup, not a blocking GUI
or helper D-Bus call. Native FD-based ACL updates preserve unrelated effective access
and directory inheritance. ACL mode grants read/traverse access at the original path;
it neither grants new write access nor projects files into the player home.
Streaming outputs and audio sinks remain available until both the game and stream processes stop.

Existing per-player Overlay data survives upgrades: correctly owned upper/work
directories from older helpers are tightened to `0700` without discarding private edits.
Session cleanup waits for stop calls to return, and stale cleanup retries cannot stop
a replacement launch. Launch manifests remain available until terminal cleanup succeeds;
recovery state is committed before manifest removal. Interrupted copy scratch directories
under SGID destination parents can be recovered without accepting unsafe ownership or access.
Helper recovery restores the complete device/runtime snapshot before persisting unit cleanup.
Main services use `couchplay-<player>.service`; stream services use
`couchplay-stream@<player>.service`, so player names cannot collide across roles.
Owned stream units using the old naming scheme are stopped during upgrade recovery.

Profiles store the application selection, not a copied directory list. Legacy directory
snapshots are removed after preserving the original profile in a sibling
`.pre-directory-snapshot.bak` file (numbered siblings preserve differing older backups).
Application edits apply on the next launch without reselecting players' applications.

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
