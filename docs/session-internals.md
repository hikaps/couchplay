# Session internals

Implementation details for how CouchPlay prepares and launches player instances. For everyday usage see the [README](../README.md); for the privileged helper see [helper-internals.md](helper-internals.md).

## Helper authorization

Normal session operations use the shared Polkit `manage-session` action (`auth_admin_keep`). Polkit may cache approval for a bounded period, but it is not permanent or guaranteed to last through the CouchPlay session. Account creation, deletion, and explicit linger changes keep their separate Polkit actions.

Managed users reload automatically when the helper is attached or becomes available; opening CouchPlay does not require a manual user refresh.

## Home alias, Heroic and launcher access

**Home alias**

- On immutable hosts such as Bazzite, Heroic configuration and shortcut copies resolve the system `/home` alias before secure copying.
- Symlinked source files and directories inside the caller's home remain rejected.

**Launcher access**

- The Flatpak shares `~/.local/share/applications` read/write, so newly generated Heroic desktop shortcuts are visible to the host helper. It does not require access to the entire home directory.
- Launcher availability is checked on the host, not against executable names inside the GUI sandbox.

**Heroic game access**

- Heroic sessions resolve current registered game roots and executable-parent directories.
- They get bounded directory traversal, without copying account credentials or per-player game settings.
- On atomic hosts, the helper admits the canonical /var/mnt storage alias only when the system /mnt path proves equivalent.

## Shared application directories

**Native application selection** adds bounded executable and working directories as read-only ACL shares.

- Session startup applies the selected application's current shared-directory settings to each player.
- Host discovery supports Flatpak installations.
- CouchPlay does not automatically share whole home folders, collection roots, credentials/configuration trees, or Flatpak runtime files.
- Sensitive home trees and canonical targets are excluded from recursive sharing.
- Application directories under /opt and /srv are supported without granting access to those roots.

Application directory settings belong to the application, not a saved player profile. Editing them applies to all profiles on their next launch; a running session keeps the values captured when it started.

## Private temporary directories

Player settings are shown directly in New Session, without a separate Advanced section.

**Private temporary directories** give that player's Gamescope/launcher/game processes separate /tmp and /var/tmp storage.

- The option defaults off and is saved per player in the profile.
- Temporary files are removed when the instance stops.
- It does not affect shared-directory settings or auxiliary Sunshine configuration.
- Install the matching GUI and host helper together; the launch D-Bus contract includes this option.

## Directory modes

| Mode | Behaviour |
| --- | --- |
| **Access original folder (ACL)** | Grants read/traverse permission at the source path; it does not project files into player HOME. |
| **Copy files** | Replaces the entire mapped player directory on every launch, discarding player edits and player-only files; Player data staging is merged before atomic publication and wins conflicts. A copy is published atomically; failure or cancellation before publication retains the previous directory. |
| **Bind** | Exposes the source only inside the player's unit namespace and preserves source permissions. |
| **Overlay** | Provides a persistent private writable layer inside that namespace while leaving the source unchanged. Filesystem/kernel ownership-mapping support is required for Overlay; unsupported sources produce an explicit error recommending Copy files. |

## Player data

**Player data** supports configured Copy/Overlay directories that receive staged files. ACL/bind shares and Steam library sharing do not qualify.

- The button opens a staging folder for the selected launcher and player, including under Flatpak.
- Each eligible directory has a generated subfolder: place files there with their relative paths preserved.
- Session startup applies staged files as part of directory preparation.
- Loose files at the staging root are ignored.
- This folder is not the player's Linux home or live /tmp.

Automation hooks run once before/after the whole session, not before each instance.

The former Config Overrides/file-pattern feature has been removed. Its old profile settings are ignored and discarded when profiles are saved; existing override files on disk are left untouched. Use configured Copy/Overlay directories and Player data for per-player configurations.

Profiles store the application selection, not a copied directory list. Legacy directory snapshots are removed after preserving the original profile in a sibling `.pre-directory-snapshot.bak` file (numbered siblings preserve differing older backups). Application edits apply on the next launch without reselecting players' applications.

## Launch environment

Launch requests support desktop integration (`XDG_CURRENT_DESKTOP`) and per-instance audio routing (`PULSE_SINK`); identity, runtime-directory, and audio-server environment values remain helper-owned. The game inherits the host locale (`LANG`, `LANGUAGE`, `LC_*`).

## Gamescope positioning

Gamescope positioning uses host-visible scripts and waits for KWin to confirm the requested frame geometry before reporting success.

If only the first instance starts and CouchPlay reports a window-positioning timeout, check KWin window discovery rather than launcher PID locks.

- Session startup matches each window to the host Gamescope PID.
- Wayland Gamescope windows can have an empty application ID.
- Gamescope can replace its initial window after a launcher loads. A PID-scoped KWin listener positions replacement windows in the same player slot until the session stops.

## Streaming internals

Streaming runs outside the helper: `couchplay-vdisplay` holds each KWin virtual output and Sunshine runs as the desktop user capturing that output with `capture = kwin`. Both are stopped when the session ends.

### Moonlight pairing

Pairing steps are in the README: [Streaming a Player to Moonlight](../README.md#streaming-a-player-to-moonlight). Implementation details:

- **Where it lives:** pairing is per gaming user, on the **Users page**. Each user's card shows the address to add in Moonlight (host address and a stable port that CouchPlay remembers for that user), the clients already paired, and "Pair Moonlight client…".
- **Later sessions:** because pairing is stored per gaming user, the client reconnects to that player's stream.
- **Deleting a user** also deletes its pairing state.
- **No session running:** "Pair Moonlight client…" starts a temporary Sunshine on a short-lived virtual display so you can enter the PIN Moonlight displays. Closing the pairing sheet stops it again.
- **Session running:** pairing runs against that player's live stream once the stream is ready.
- **Unpair:** "Unpair" next to a client removes it the same way.
- **Session page:** it only shows each player's pairing status and a "Manage on Users page" link.
- **Sunshine access:** CouchPlay talks to Sunshine's local web API and trusts only the certificate stored in the user's Sunshine state directory.

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

The Start + Select exit chord works with standard gamepads (Xbox, PlayStation, generic HID) and with the Valve Steam Controller, even when Steam is active and holding the evdev node, since CouchPlay reads Steam Controller input directly from the raw HID (`/dev/hidrawN`) endpoint.

## Profile deletion UI

On the **Profiles** page, card actions wrap on narrow windows so **Delete** remains accessible; deleting a profile requires confirmation.
