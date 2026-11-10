# Helper internals

Implementation details of the privileged helper and the instance units it manages. For everyday usage see the [README](../README.md); for session preparation see [session-internals.md](session-internals.md).

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

## Cross-user audio (PulseAudio TCP listener)

Secondary players reach the compositor user's PipeWire through `tcp:127.0.0.1:4713`. `SetupRuntimeAccess` makes sure that listener exists without restarting the user's `pipewire-pulse`, so the desktop user's running audio streams are not interrupted:

- The helper runs `pactl` as the compositor user through `machinectl shell <user>@ /usr/bin/pactl …` (structured argv, no shell).
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
