# Helper internals

Implementation details of the privileged helper and the instance units it manages. For everyday usage see the [README](../README.md); for session preparation see [session-internals.md](session-internals.md).

## Flatpak path mapping

Flatpak directory selections are resolved from document-portal exports to host paths before being saved as shared directories. The GUI and helper share one mapping rule: strict descendants of either the account's declared home or its canonical path are home-relative. External paths remain external; traversal and whole-home sharing without an explicit target alias are rejected. This applies to any symlinked home, including `/home` → `/var/home` on Atomic desktops. Both home paths are resolved by the host helper, not inside the Flatpak sandbox. Update the GUI and helper together when installing this fix. Secure path walks require only search permission on ancestor directories and retain read checks on the selected directory. Explicitly projected sources, including symlinked Steam libraries, are resolved on the host and revalidated before no-follow pinning. Implicit target routing and staging directories retain strict no-follow path walks.

Player staging directories are resolved to canonical paths before helper handoff, including linked application-data storage. The helper still requires caller-owned home storage and no-follow traversal of the resolved directories and staged files.

## Mounts and Overlay recovery

Bind and Overlay views exist in each instance's private mount namespace, not in the host file manager or helper service. Startup reports preparation errors before Gamescope window discovery. Stop all sessions before updating the matching GUI and native helper. Directory sources are carried by the preparation plan, independently of working-directory admission.

Detached source mounts are made private before attachment, so shared host mounts cannot propagate a player Overlay back onto the original source directory. If an older build leaked an Overlay, inspect the mount before changing ownership: the player-owned view may be hiding an unchanged host directory. After uncovering the host directory, restart the helper so its private namespace does not retain the old leaked view.

Gamescope is projected read-only with `nosuid` inside each instance so distro file capabilities do not prevent an unprivileged launch; the host binary is unchanged.

Existing per-player Overlay data survives upgrades: correctly owned upper/work directories from older helpers are tightened to `0700` without discarding private edits.

## ACL preparation

Recursive ACL preparation runs in cancellable instance startup, not a blocking GUI or helper D-Bus call. Native FD-based ACL updates preserve unrelated effective access and directory inheritance. ACL mode grants read/traverse access at the original path; it neither grants new write access nor projects files into the player home.

ACL shares on read-only filesystems are accepted only when the player already has read/execute access to every object; otherwise preparation fails.

## Cleanup and recovery

Session cleanup waits for stop calls to return, and stale cleanup retries cannot stop a replacement launch. Launch manifests remain available until terminal cleanup succeeds; recovery state is committed before manifest removal. Interrupted copy scratch directories under SGID destination parents can be recovered without accepting unsafe ownership or access. Helper recovery restores the complete device/runtime snapshot before persisting unit cleanup.

## Instance units

Instance units use `couchplay-<player>.service`. They keep a 60 second start timeout. While directory preparation makes progress (entries or copy chunks processed), the bootstrap asks systemd to extend it every 10 seconds, so large copies finish while a stalled preparation still times out.
