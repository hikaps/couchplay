# Session internals

Implementation details for how CouchPlay prepares and launches player instances. For everyday usage see the [README](../README.md).

## Sandbox model

CouchPlay is rootless. A player is a directory (`players/<name>/home`, mode 0700) under `~/.local/share/couchplay/players` (under Flatpak: `AppDataLocation/players`). Each instance runs as the desktop user inside `bwrap → dbus-run-session → gamescope → game`:

- private HOME (the player's `home` directory), private `/tmp`, and a private session bus (the host session bus socket and the systemd user manager's private socket are masked, so `flatpak-spawn` and `systemd-run --user` escapes are blocked; the host's system bus socket is masked too, so the sandbox cannot call host system services);
- the host network (LAN, local and online play between players work as before) and a private PID namespace (stopping gamescope tears down everything);
- a fresh `/dev` with only GPU, audio and fuse nodes, that player's virtual pad and any keyboards or mice assigned to that player; `/dev/uinput` and `hidraw` nodes are not visible.

Controllers: `couchplay-padfwd` creates one virtual Xbox 360 pad per player and forwards the assigned physical pads to it (with rumble). The virtual pad survives reconnects. It needs a user-writable `/dev/uinput`. `couchplay-vdisplay` and `couchplay-padfwd` run on the host; under Flatpak they run straight from the Flatpak's app path.

Host requirements: `bubblewrap`, `dbus` (`dbus-run-session`), `gamescope`. `SessionRunner` refuses to start with a clear message when one is missing. Managed players are listed straight from the player directory; no refresh is needed.

## Home alias, Heroic and launcher access

**Home alias**

- On immutable hosts such as Bazzite, Heroic configuration and shortcut copies resolve the system `/home` alias before copying.
- Symlinks are recreated as symlinks and never followed when copying.

**Launcher access**

- The Flatpak shares `~/.local/share/applications` read/write, so newly generated Heroic desktop shortcuts are visible on the host. It does not require access to the entire home directory.
- Launcher availability is checked on the host, not against executable names inside the GUI sandbox.

**Heroic game access**

- Heroic sessions resolve current registered game roots and executable-parent directories.
- Those under your home are bound read-only; others are already visible. Account credentials and per-player game settings are not copied.
- On atomic hosts, the canonical /var/mnt storage alias is admitted only when the system /mnt path proves equivalent.

## Shared application directories

**Native application selection** adds bounded executable and working directories as read-only binds when they lie under your home (anything outside home is already visible in the sandbox).

- Session startup applies the selected application's current shared-directory settings to each player.
- Host discovery supports Flatpak installations.
- CouchPlay does not automatically share whole home folders, collection roots, credentials/configuration trees, or Flatpak runtime files.
- Sensitive home trees and canonical targets are excluded from recursive sharing.

Application directory settings belong to the application, not a saved player profile. Editing them applies to all profiles on their next launch; a running session keeps the values captured when it started.

## Private temporary directories

`/tmp` is always private per player (a fresh tmpfs inside the sandbox) and discarded when the instance stops. The former per-profile option was removed; old `privateTemporaryDirectories` keys are dropped when a profile is saved.

## Directory modes

| Mode | Behaviour |
| --- | --- |
| **Access original folder (read-only)** (stored as `acl`) | Read-only bind of the source at the same path when it lies under your home; paths outside home are already visible. It does not project files into player HOME. |
| **Copy files** | Replaces the mapped directory in the player's home on every launch, discarding player edits and player-only files; Player data staging is merged before the swap and wins conflicts. The copy is built next to the target and renamed into place, so a failed copy leaves the previous directory intact. |
| **Bind** | Read-write bind of the source inside the player's sandbox. |
| **Overlay** | Persistent private writable layer (`players/<name>/layers/<key>/{upper,work}`) over the source inside the sandbox; the source stays unchanged. Staged Player data is copied into the upper layer before launch. |

## Player data

**Player data** supports configured Copy/Overlay directories that receive staged files. Read-only access, bind shares and Steam library sharing do not qualify.

- The button opens a staging folder for the selected launcher and player, including under Flatpak.
- Each eligible directory has a generated subfolder: place files there with their relative paths preserved.
- Session startup applies staged files as part of directory preparation.
- Loose files at the staging root are ignored.
- This folder is not the player's home or live /tmp.

Automation hooks run once before/after the whole session, not before each instance.

The former Config Overrides/file-pattern feature has been removed. Its old profile settings are ignored and discarded when profiles are saved; existing override files on disk are left untouched. Use configured Copy/Overlay directories and Player data for per-player configurations.

Profiles store the application selection, not a copied directory list. Legacy directory snapshots are removed after preserving the original profile in a sibling `.pre-directory-snapshot.bak` file (numbered siblings preserve differing older backups). Application edits apply on the next launch without reselecting players' applications.

## Launch environment

Launch requests support desktop integration (`XDG_CURRENT_DESKTOP`) and per-instance audio routing (`PULSE_SINK`). The game inherits the host locale (`LANG`, `LANGUAGE`, `LC_*`). Each instance's environment also carries `COUCHPLAY_SANDBOX_ID`, a random token used to find the gamescope PID.

Players share the host network, so LAN, direct-IP, local and online lobbies work between them. Steam's fixed IPC port `57343` would collide, so each Steam-launcher player gets a private Steam IPC address: a free loopback port that `SessionRunner` reserves at session start and sets as `steam_master_ipc_name_override=127.0.0.1:<port>` in the environment of that player's Steam and its games. Steam honors the variable for its own listener, so no command-line flag is needed. Without it, every player's Steam and games share port `57343`, so games reach the desktop's (or another player's) Steam and crash in `steamclient.so` within seconds. CouchPlay deliberately does not pass `-master_ipc_name_override` on Steam's command line: Halo Infinite crashes at its Xbox sign-in ("Getting profile from platform") whenever Steam is started with the flag, even with a matching environment. (A running Steam shows the flag in its own `ps` entry because it re-launches itself with it.)

## Gamescope positioning

Gamescope positioning uses host-visible scripts and waits for KWin to confirm the requested frame geometry before reporting success.

If only the first instance starts and CouchPlay reports a window-positioning timeout, check KWin window discovery rather than launcher PID locks.

- Session startup matches each window to the host Gamescope PID.
- Wayland Gamescope windows can have an empty application ID.
- Gamescope can replace its initial window after a launcher loads. A PID-scoped KWin listener positions replacement windows in the same player slot until the session stops.

## Streaming internals

Streaming runs on the host: `couchplay-vdisplay` holds each KWin virtual output and Sunshine runs as the desktop user capturing that output with `capture = kwin`. Both are stopped when the session ends.

### Moonlight pairing

Pairing steps are in the README: [Streaming a Player to Moonlight](../README.md#streaming-a-player-to-moonlight). Implementation details:

- **Where it lives:** pairing is per player, on the **Players page**. Each player's card shows the address to add in Moonlight (host address and a stable port that CouchPlay remembers for that player), the clients already paired, and "Pair Moonlight client…".
- **Later sessions:** because pairing is stored per player, the client reconnects to that player's stream.
- **Deleting a player** also deletes its pairing state (the Sunshine state directory).
- **No session running:** "Pair Moonlight client…" starts a temporary Sunshine on a short-lived virtual display so you can enter the PIN Moonlight displays. Closing the pairing sheet stops it again.
- **Session running:** pairing runs against that player's live stream once the stream is ready.
- **Unpair:** "Unpair" next to a client removes it the same way.
- **Session page:** it only shows each player's pairing status and a "Manage on Players page" link.
- **Sunshine access:** CouchPlay talks to Sunshine's local web API and trusts only the certificate stored in the player's Sunshine state directory.

## Add to Steam shortcuts

**Account discovery**

- Native Steam and Flatpak Steam are supported, including the `~/.var/app/com.valvesoftware.Steam/data/Steam` directory.
- Shortcut files that exceed the supported byte or record limits are left unchanged.
- Symlinked shortcut and backup files, including dangling links, are rejected without writing to their targets.

**Shortcut behaviour**

- The shortcut launches the installed `couchplay-gamemode` command with `--profile NAME --start --exit-after-session`, starting the saved profile and its configured games.
- If CouchPlay is already open, close it first: the current singleton does not forward shortcut arguments to an existing instance.
- Reopening Steam refreshes its shortcut list; it does not switch accounts.

## Controller exit chord

The Start + Select exit chord is detected by `couchplay-padfwd` on the physical sources attached to a player's virtual pad (held for 2 s). The Valve Steam Controller works through its kernel `hid-steam` evdev gamepad node; raw `/dev/hidraw*` access is not available inside the sandbox.

## Profile deletion UI

On the **Profiles** page, card actions wrap on narrow windows so **Delete** remains accessible; deleting a profile requires confirmation.
