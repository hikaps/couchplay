# AGENTS.md - Privileged Helper Service Guidelines

## OVERVIEW

D-Bus privileged service for split-screen gaming: user creation, device ownership, IPC orchestration (2 files, 1910 lines)

## STRUCTURE

**CouchPlayHelper.cpp (1580 lines):** Main service implementation - Polkit authorization, D-Bus slots, machinectl process spawning, device ownership, mount management

**CouchPlayHelper.h (330 lines):** D-Bus interface definition - 15 Q_SLOT methods exposed via io.github.hikaps.CouchPlayHelper

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| D-Bus interface | CouchPlayHelper.h:30 | Q_CLASSINFO declares D-Bus interface name |
| User creation | CouchPlayHelper.cpp:CreateUser() | useradd + systemd linger + group add |
| Device ownership | CouchPlayHelper.cpp:ChangeDeviceOwner() | chown /dev/input/event* to gaming user |
| Process spawning | CouchPlayHelper.cpp:LaunchInstance() | machinectl shell --user command |
| Mount management | CouchPlayHelper.cpp:MountSharedDirectories() | bind-mount for shared game directories |
| ACL management | CouchPlayHelper.cpp:SetRuntimeAccess() | setfacl on wayland-0, pipewire-0 sockets |
| Authorization | CouchPlayHelper.cpp:658 | TODO: Implement proper PolicyKit check |

## CONVENTIONS

**D-Bus Pattern:**
- Q_CLASSINFO("D-Bus Interface", "io.github.hikaps.CouchPlayHelper")
- public Q_SLOTS: all methods exposed via D-Bus
- QDBusContext: inherited for checking caller identity

**Privileged Operations:**
- User management via QProcess calling useradd/userdel/usermod
- Device ownership via QProcess calling chown/chgrp
- Mount operations via QProcess calling mount/umount
- ACL operations via QProcess calling setfacl/getfacl

**Resource Tracking:**
- m_modifiedDevices: QStringList of changed device paths
- m_launchedProcesses: QMap<PID, QProcess*> for spawned instances
- m_activeMounts: QMap<username, QList<MountInfo>> for tracking mounts
- Cleanup in destructor: reset all devices, unmount all mounts

**Error Handling:**
- Return false/0 for failure, true/non-zero for success
- qWarning() for logged errors (helper runs as daemon)
- No user-facing UI - all errors return to GUI via D-Bus

## ANTI-PATTERNS

- **No Polkit authorization**: checkAuthorization() stub returns true (TODO:658)
- **Process blocking**: QProcess::waitForFinished() used synchronously (acceptable for daemon)
- **No signal/slot IPC**: D-Bus only - no Qt signals across process boundary
- **Hardcoded paths**: Uses /usr/sbin/useradd, /usr/bin/mount directly
- **No rate limiting**: CreateUser/DeleteUser unlimited (should have guard rails)

## UNIQUE PATTERNS

**machinectl Spawning:** LaunchInstance() uses `machinectl shell --user <username> -- bash -c <command>` to run gamescope as any user while inheriting compositor's Wayland session.

**Runtime ACL Dance:** SetRuntimeAccess() grants couchplay group read+execute on XDG_RUNTIME_DIR sockets (wayland-0, pipewire-0) so secondary players can access compositor resources.

**Mount Aliasing:** MountSharedDirectories() supports both home-relative paths (mounted at same relative location) and absolute paths with explicit aliases or ~/.couchplay/mounts/ default.

**Cleanup Contract:** Helper destructor removes all device ownership changes and unmounts all mounts - ensures no lingering privileged state after app exit.

## NOTES

- **D-Bus service name:** io.github.hikaps.CouchPlayHelper (see data/dbus/)
- **Object path:** /io/github/hikaps/CouchPlayHelper
- **System service:** Runs as root via systemd (data/dbus/couchplay-helper.service)
- **Polkit actions:** Defined in data/polkit/io.github.hikaps.couchplay.policy
- **No GUI:** Helper is headless daemon - all UI in main couchplay app
