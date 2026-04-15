# AGENTS.md - Privileged Helper Service Guidelines

## OVERVIEW

D-Bus privileged service for split-screen gaming: user creation, device ownership, IPC orchestration, state persistence (2 files, 2320 lines)

## STRUCTURE

**CouchPlayHelper.cpp (1945 lines):** Main service implementation - Polkit authorization, D-Bus slots, systemd-run transient unit spawning, device ownership, mount management, state persistence, runtime ACL cache verification, dynamic gamescope path resolution

**CouchPlayHelper.h (375 lines):** D-Bus interface definition - 15 Q_SLOT methods exposed via io.github.hikaps.CouchPlayHelper

**SystemOps.h/cpp:** Abstraction layer for system calls (getpwnam, chown, mount, etc.) - enables mocking in tests

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| D-Bus interface | CouchPlayHelper.h:30 | Q_CLASSINFO declares D-Bus interface name |
| User creation | CouchPlayHelper.cpp:CreateUser() | useradd + systemd linger + group add |
| Device ownership | CouchPlayHelper.cpp:ChangeDeviceOwner() | chown /dev/input/event* to gaming user |
| Process spawning | CouchPlayHelper.cpp:LaunchInstance() | systemd-run --uid transient unit launch |
| Mount management | CouchPlayHelper.cpp:MountSharedDirectories() | bind-mount for shared game directories |
| ACL management | CouchPlayHelper.cpp:SetRuntimeAccess() | setfacl on wayland-0, pipewire-0 sockets |
| Runtime ACL cache | CouchPlayHelper.cpp:loadAndReconcileState() | Verifies ACLs on wayland-0 via getfacl, evicts stale UIDs |
| Dynamic gamescope path | CouchPlayHelper.cpp:startTransientUnit() | Falls back to `which gamescope` if /usr/bin/gamescope missing |
| Validation helpers | CouchPlayHelper.cpp:validateUserAndAuth() | Shared username+auth+userExists check (12 call sites) |
| Process helper | CouchPlayHelper.cpp:runCommand() | Shared QProcess spawn/await (8 call sites) |
| ACL cleanup | CouchPlayHelper.cpp:removeRuntimeAcls() | Shared method for runtime directory ACL reset |
| State persistence | CouchPlayHelper.cpp:saveState()/loadAndReconcileState() | JSON at /run/couchplay/state.json |
| Authorization | CouchPlayHelper.cpp:658 | TODO: Implement proper PolicyKit check |

## CONVENTIONS

**D-Bus Pattern:** Q_CLASSINFO declares interface name. public Q_SLOTS exposed via D-Bus. QDBusContext inherited for caller identity.

**Privileged Operations:** All via QProcess calling useradd/userdel/usermod, chown/chgrp, mount/umount, setfacl/getfacl.

**Resource Tracking:** m_modifiedDevices (changed device paths), m_usernameToUnitName (transient units). Destructor resets all devices + unmounts all mounts.

**Error Handling:** Return false/0 for failure. qWarning() for logged errors. No UI, all errors return via D-Bus.

## ANTI-PATTERNS

- **No Polkit authorization**: checkAuthorization() stub returns true (TODO:658)
- **Process blocking**: QProcess::waitForFinished() used synchronously (acceptable for daemon)
- **No signal/slot IPC**: D-Bus only - no Qt signals across process boundary
- **Hardcoded paths**: Uses /usr/sbin/useradd, /usr/bin/mount directly
- **No rate limiting**: CreateUser/DeleteUser unlimited (should have guard rails)

## UNIQUE PATTERNS

**systemd-run Spawning:** LaunchInstance() uses `systemd-run --uid <username> --unit couchplay-<username>.service` to launch gamescope as any user in a transient systemd unit.

**Runtime ACL Dance:** SetRuntimeAccess() grants couchplay group read+execute on XDG_RUNTIME_DIR sockets (wayland-0, pipewire-0) so secondary players can access compositor resources.

**Mount Aliasing:** MountSharedDirectories() supports both home-relative paths (mounted at same relative location) and absolute paths with explicit aliases or ~/.couchplay/mounts/ default.

**Cleanup Contract:** Destructor removes all device ownership changes and unmounts all mounts, ensuring no lingering privileged state.

**IPC Cleanup on DeleteUser:** Removes semaphores, shared memory, message queues owned by user to prevent "Permission denied" errors if new user gets same name.

**Reverse Mount Unmounting:** Processes mounts in reverse order for nested mount handling. Falls back to `umount -l` on failure.

**Runtime ACL Verification:** On helper restart, validates cached runtime UIDs by checking actual ACLs on wayland socket via getfacl. Evicts stale entries to force re-application.

**Dynamic Gamescope Resolution:** Checks /usr/bin/gamescope first, falls back to `which gamescope` for immutable distros where gamescope lives outside /usr/bin.

**Stale Unit Recovery:** When systemd-run fails with "already loaded", stops + resets the failed unit, then retries after 200ms.

**Remove-Before-Set ACL Pattern:** Before setting ACLs (setfacl -m), removes stale entries (setfacl -x) to avoid "Duplicate entries" errors from unresolved GIDs.

## NOTES

- **D-Bus service name:** io.github.hikaps.CouchPlayHelper (see data/dbus/)
- **Object path:** /io/github/hikaps/CouchPlayHelper
- **System service:** Runs as root via systemd (data/dbus/couchplay-helper.service)
- **Polkit actions:** Defined in data/polkit/io.github.hikaps.couchplay.policy
- **No GUI:** Helper is headless daemon - all UI in main couchplay app
