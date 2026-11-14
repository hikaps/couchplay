# Helper internals

Implementation details of the privileged helper and the instance units it manages. For everyday usage see the [README](../README.md); for session preparation see [session-internals.md](session-internals.md).

## Version and API level

The read-only `GetVersionInfo()` method (no Polkit) returns a map with `version` (the helper build version) and `apiLevel` (`COUCHPLAY_HELPER_API_LEVEL` in `helper/HelperApiLevel.h`). Bump the level whenever a D-Bus change lands that a GUI built against it cannot tolerate from an older helper.

`CouchPlayHelperClient` exposes `helperVersion`, `helperApiLevel`, `requiredHelperApiLevel` and `helperOutdated`. Helpers older than 0.6.1 lack the method; an error reply is mapped to an empty version and level 0. `helperOutdated` is true only when the helper is available and its level is below the level the GUI requires (currently 0, so it does not fire yet). Raise `requiredHelperApiLevel` in the client when the GUI starts depending on a newer helper.

## Flatpak path mapping

Flatpak directory selections are resolved from document-portal exports to host paths before being saved as shared directories.

**Mapping rule** (shared by the GUI and helper):

- Strict descendants of either the account's declared home or its canonical path are home-relative.
- External paths remain external.
- Traversal and whole-home sharing without an explicit target alias are rejected.
- This applies to any symlinked home, including `/home` → `/var/home` on Atomic desktops.
- Both home paths are resolved by the host helper, not inside the Flatpak sandbox.
- Update the GUI and helper together when installing this fix.

**Path walks**

- Secure path walks require only search permission on ancestor directories and retain read checks on the selected directory.
- Explicitly projected sources, including symlinked Steam libraries, are resolved on the host and revalidated before no-follow pinning.
- Implicit target routing and staging directories retain strict no-follow path walks.

Player staging directories are resolved to canonical paths before helper handoff, including linked application-data storage. The helper still requires caller-owned home storage and no-follow traversal of the resolved directories and staged files.

## Mounts and Overlay recovery

**Mount view**

- Bind and Overlay views exist in each instance's private mount namespace, not in the host file manager or helper service.
- Startup reports preparation errors before Gamescope window discovery.
- Directory sources are carried by the preparation plan, independently of working-directory admission.
- Stop all sessions before updating the matching GUI and native helper.

**Detached source mounts** are made private before attachment, so shared host mounts cannot propagate a player Overlay back onto the original source directory.

If an older build leaked an Overlay:

- Inspect the mount before changing ownership: the player-owned view may be hiding an unchanged host directory.
- After uncovering the host directory, restart the helper so its private namespace does not retain the old leaked view.

Gamescope is projected read-only with `nosuid` inside each instance so distro file capabilities do not prevent an unprivileged launch; the host binary is unchanged.

Existing per-player Overlay data survives upgrades: correctly owned upper/work directories from older helpers are tightened to `0700` without discarding private edits.

## ACL preparation

Recursive ACL preparation runs in cancellable instance startup, not a blocking GUI or helper D-Bus call. Native FD-based ACL updates preserve unrelated effective access and directory inheritance. ACL mode grants read/traverse access at the original path; it neither grants new write access nor projects files into the player home.

ACL shares on read-only filesystems are accepted only when the player already has read/execute access to every object; otherwise preparation fails.

## Controller ownership

A chown does not revoke handles other users already hold, so the desktop's Steam or launchers could keep reading a player's pad (and Steam re-emits it as its own virtual pad). Physical controllers are therefore rebound under a temporary udev rule that gives the new nodes to the player:

- **HID controllers** are rebound through their `hidraw` node.
- **USB gamepads driven directly by an interface driver** such as `xpad` (Xbox 360-protocol pads) have no `hidraw` node. Their event node resolves to the USB interface (for example `1-5.2:1.0`), which is rebound instead. Only joystick nodes on a USB interface qualify, never hubs or whole devices.
- After the rebind the helper resolves the recreated node and watches it for the exit chord. The event and joystick nodes may come back with new numbers, and another device can take an old number. For the rest of the session, ownership and reset requests for the old paths therefore go to the recreated nodes, since the GUI still lists the paths it saw before the rebind.
- Session reset removes the rule and rebinds again, returning the nodes to the seat user.
- Other nodes (virtual devices, keyboards and mice without `hidraw`) are only chowned.

## Steam virtual pads

With Steam Input on, each player's Steam re-creates their pad as a uinput device, and games read only that device. udev gives new input nodes to the seat user, so the player could not open their own virtual pad. When the `/dev/input` watcher sees a new virtual device during a session, the helper looks up which process holds the `/dev/uinput` fd that created it (`pidfd_getfd` plus `UI_GET_SYSNAME`). If that process belongs to a session player, the helper gives the event and joystick nodes to the player (mode 0600, ACLs cleared). Virtual pads created by the desktop user's Steam are left alone. Event nodes are tracked with the other modified devices and reset when the session stops.

## Cross-user audio (PulseAudio TCP listener)

Secondary players reach the compositor user's PipeWire through `tcp:127.0.0.1:4713`. `SetupRuntimeAccess` makes sure that listener exists without restarting the user's `pipewire-pulse`, so the desktop user's running audio streams are not interrupted:

- The helper runs `pactl` as the compositor user through `setpriv --reuid … --init-groups -- /usr/bin/env -i HOME=… XDG_RUNTIME_DIR=/run/user/<uid> /usr/bin/pactl …` (structured argv, no shell). `machinectl shell` is not used because it does not return the command's exit status.
- If `pactl list short modules` already shows a `module-native-protocol-tcp` with `port=4713`, or the port already answers (for example a `pipewire-pulse` drop-in), nothing is loaded.
- Otherwise it runs `pactl load-module module-native-protocol-tcp port=4713 listen=127.0.0.1 auth-anonymous=1`. The compositor UID is remembered as helper-loaded.
- When the last instance of that compositor goes away (or `RemoveRuntimeAccess`/helper shutdown runs), only a helper-loaded module with exactly those arguments is removed with `pactl unload-module`. Pre-existing modules and drop-ins are never touched.
- A `~/.config/pipewire/pipewire-pulse.conf.d/50-couchplay.conf` written by CouchPlay 0.6.0 is deleted during cleanup, without restarting `pipewire-pulse`; it is inert until the next natural restart.
- A failed load is logged as a warning; the socket ACLs are still applied and runtime access setup continues.

## Cleanup and recovery

**Stop and retry**

- Session cleanup waits for stop calls to return.
- Stale cleanup retries cannot stop a replacement launch.

**Manifests and recovery**

- Launch manifests remain available until terminal cleanup succeeds; recovery state is committed before manifest removal.
- Interrupted copy scratch directories under SGID destination parents can be recovered without accepting unsafe ownership or access.
- Helper recovery restores the complete device/runtime snapshot before persisting unit cleanup.

## Instance units

Instance units use `couchplay-<player>.service`. They keep a 60 second start timeout. While directory preparation makes progress (entries or copy chunks processed), the bootstrap asks systemd to extend it every 10 seconds, so large copies finish while a stalled preparation still times out.
