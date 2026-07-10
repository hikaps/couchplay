# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

"""Mock CouchPlayHelper D-Bus service for e2e testing.

Implements the io.github.hikaps.CouchPlayHelper interface exactly as the real
helper (helper/CouchPlayHelper.h) exposes it, so the GUI cannot tell the
difference — but LaunchInstance() returns a fake PID and never spawns gamescope.

Call recording: every LaunchInstance() invocation is appended (JSON-per-line)
to the path in COUCHPLAY_MOCK_LAUNCH_LOG (default /tmp/couchplay-mock-launch.jsonl)
so multi-instance assertions can verify N distinct launches with the right
per-user command/args. Tests that don't care ignore the file.
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path
import grp
import pwd

import dbus
import dbus.service

BUS_NAME = "io.github.hikaps.CouchPlayHelper"
OBJECT_PATH = "/io/github/hikaps/CouchPlayHelper"
INTERFACE_NAME = "io.github.hikaps.CouchPlayHelper"

TEST_USERS = ["player2", "player3"]
LAUNCH_LOG = os.environ.get(
    "COUCHPLAY_MOCK_LAUNCH_LOG", "/tmp/couchplay-mock-launch.jsonl"
)
# When set, the mock returns plausible uids WITHOUT touching useradd/userdel --
# required for rootless containers (no sudo) and avoids any user leak. The app
# only uses the uid for downstream helper calls (SetupRuntimeAccess,
# ChangeDeviceOwner, ...) which are all mocked to succeed, so a fake uid is fine.
FAKE_USERS = os.environ.get("COUCHPLAY_MOCK_FAKE_USERS") == "1"
_FAKE_UIDS = {"player2": 10011, "player3": 10012}


def _fake_uid(username):
    if username in _FAKE_UIDS:
        return _FAKE_UIDS[username]
    # deterministic, stable across calls for the same username
    return 10000 + (sum(ord(c) for c in username) % 9000)


class MockHelper(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, OBJECT_PATH)
        self._next_pid = 10000
        self._launched_pids = set()
        self._created_users = {}
        # Truncate any stale launch log from a previous run.
        try:
            open(LAUNCH_LOG, "w").close()
        except OSError:
            pass

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="u")
    def CreateUser(self, username, fullname):
        if FAKE_USERS:
            uid = _fake_uid(username)
            self._created_users[username] = uid
            return uid
        try:
            subprocess.run(
                ["groupadd", "-f", "couchplay"], check=False, capture_output=True
            )
            subprocess.run(
                ["useradd", "-m", "-G", "couchplay", "-c", fullname, username],
                check=True,
                capture_output=True,
            )
            subprocess.run(
                ["loginctl", "enable-linger", username],
                check=False,
                capture_output=True,
            )
            uid = _get_uid(username)
            self._created_users[username] = uid
            return uid
        except subprocess.CalledProcessError:
            return 0

    @dbus.service.method(INTERFACE_NAME, in_signature="sb", out_signature="b")
    def DeleteUser(self, username, removeHome):
        if FAKE_USERS:
            self._created_users.pop(username, None)
            return True
        try:
            subprocess.run(
                ["userdel"] + (["-r"] if removeHome else []) + [username],
                check=True,
                capture_output=True,
            )
            self._created_users.pop(username, None)
            return True
        except subprocess.CalledProcessError:
            return False

    @dbus.service.method(INTERFACE_NAME, in_signature="s", out_signature="b")
    def IsInCouchPlayGroup(self, username):
        result = subprocess.run(["groups", username], capture_output=True, text=True)
        return "couchplay" in result.stdout

    @dbus.service.method(INTERFACE_NAME, in_signature="s", out_signature="b")
    def EnableLinger(self, username):
        result = subprocess.run(
            ["loginctl", "enable-linger", username],
            capture_output=True,
        )
        return result.returncode == 0

    @dbus.service.method(INTERFACE_NAME, in_signature="s", out_signature="b")
    def IsLingerEnabled(self, username):
        result = subprocess.run(
            ["loginctl", "show-user", username, "Linger"],
            capture_output=True,
            text=True,
        )
        return "yes" in result.stdout

    @dbus.service.method(INTERFACE_NAME, in_signature="u", out_signature="b")
    def SetupRuntimeAccess(self, compositorUid):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="u", out_signature="b")
    def RemoveRuntimeAccess(self, compositorUid):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="su", out_signature="b")
    def ChangeDeviceOwner(self, devicePath, uid):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="asu", out_signature="i")
    def ChangeDeviceOwnerBatch(self, devicePaths, uid):
        return len(devicePaths)

    @dbus.service.method(INTERFACE_NAME, in_signature="s", out_signature="b")
    def ResetDeviceOwner(self, devicePath):
        return True

    @dbus.service.method(INTERFACE_NAME, out_signature="i")
    def ResetAllDevices(self):
        return 0

    # NOTE: signature must match helper/CouchPlayHelper.h LaunchInstance —
    # 6 args: username(s) compositorUid(u) gamescopeArgs(as) gameCommand(s)
    # environment(as) bindPaths(as) -> pid(x).
    @dbus.service.method(
        INTERFACE_NAME,
        in_signature="suassasas",
        out_signature="x",
    )
    def LaunchInstance(
        self, username, compositorUid, gamescopeArgs, gameCommand, environment, bindPaths
    ):
        pid = self._next_pid
        self._next_pid += 1
        self._launched_pids.add(pid)
        _record_launch(
            pid=pid,
            username=str(username),
            compositorUid=int(compositorUid),
            gamescopeArgs=[str(a) for a in gamescopeArgs],
            gameCommand=str(gameCommand),
            environment=[str(e) for e in environment],
            bindPaths=[str(b) for b in bindPaths],
        )
        return pid

    @dbus.service.method(INTERFACE_NAME, in_signature="x", out_signature="b")
    def StopInstance(self, pid):
        self._stop(pid)
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="x", out_signature="b")
    def KillInstance(self, pid):
        self._stop(pid)
        return True

    def _stop(self, pid):
        self._launched_pids.discard(int(pid))

    @dbus.service.method(INTERFACE_NAME, in_signature="suas", out_signature="i")
    def MountSharedDirectories(self, username, compositorUid, directories):
        return len(directories)

    @dbus.service.method(INTERFACE_NAME, in_signature="s", out_signature="i")
    def UnmountSharedDirectories(self, username):
        return 0

    @dbus.service.method(INTERFACE_NAME, out_signature="i")
    def UnmountAllSharedDirectories(self):
        return 0

    @dbus.service.method(INTERFACE_NAME, in_signature="sss", out_signature="b")
    def CopyFileToUser(self, sourcePath, targetPath, username):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="b")
    def CreateUserDirectory(self, path, username):
        try:
            Path(path).mkdir(parents=True, exist_ok=True)
            return True
        except OSError:
            return False

    @dbus.service.method(INTERFACE_NAME, in_signature="ssb", out_signature="b")
    def SetDirectoryAcl(self, path, username, recursive):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="b")
    def SetPathAclWithParents(self, path, username):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="s", out_signature="s")
    def GetUserSteamId(self, username):
        return ""

    @dbus.service.method(INTERFACE_NAME, in_signature="", out_signature="as")
    def ListCouchPlayUsers(self):
        entries = []
        for username in self._couchplayUsernames():
            fields = self._userFields(username)
            if fields is None:
                continue
            name, uid, gid, home, shell = fields
            if uid < 1000 or uid >= 65534:
                continue
            if "nologin" in shell or "false" in shell:
                continue
            if not os.path.isdir(home):
                continue
            entries.append("%s\t%d\t%d\t%s\t%s" % (name, uid, gid, home, shell))
        return dbus.Array(entries, signature="s")

    @dbus.service.method(INTERFACE_NAME, in_signature="s", out_signature="a{sv}")
    def GetUserInfo(self, username):
        fields = self._userFields(username)
        if fields is None:
            return dbus.Dictionary({}, signature="sv")
        name, uid, gid, home, shell = fields
        return dbus.Dictionary({
            "uid": dbus.UInt32(uid),
            "gid": dbus.UInt32(gid),
            "home": home,
        }, signature="sv")

    def _couchplayUsernames(self):
        if FAKE_USERS:
            return list(self._created_users.keys())
        try:
            return list(grp.getgrnam("couchplay").gr_mem)
        except KeyError:
            return []

    def _userFields(self, username):
        if FAKE_USERS:
            uid = self._created_users.get(username)
            if uid is None:
                return None
            return (username, uid, uid, "/home/%s" % username, "/bin/bash")
        try:
            p = pwd.getpwnam(username)
        except KeyError:
            return None
        return (p.pw_name, p.pw_uid, p.pw_gid, p.pw_dir, p.pw_shell)

    @dbus.service.method(INTERFACE_NAME, in_signature="ayss", out_signature="b")
    def WriteFileToUser(self, content, targetPath, username):
        return True

    # --- Streaming (PR #22): signatures mirror helper/CouchPlayHelper.h ---
    @dbus.service.method(INTERFACE_NAME, in_signature="siii", out_signature="s")
    def CreateVirtualOutput(self, username, width, height, refreshRate):
        socket = "wayland-mock-%dx%d" % (int(width), int(height))
        _record_launch(
            method="CreateVirtualOutput",
            username=str(username),
            width=int(width),
            height=int(height),
            refreshRate=int(refreshRate),
            socket=socket,
        )
        return socket

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="b")
    def DestroyVirtualOutput(self, username, waylandSocketName):
        _record_launch(
            method="DestroyVirtualOutput",
            username=str(username),
            socket=str(waylandSocketName),
        )
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="s")
    def CreateNullSink(self, username, sinkName):
        _record_launch(
            method="CreateNullSink",
            username=str(username),
            sinkName=str(sinkName),
        )
        return str(sinkName)

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="b")
    def DestroyNullSink(self, username, sinkName):
        _record_launch(
            method="DestroyNullSink",
            username=str(username),
            sinkName=str(sinkName),
        )
        return True

    def cleanup(self):
        if FAKE_USERS:
            self._created_users.clear()
            return
        for username in list(self._created_users.keys()):
            self.DeleteUser(username, True)


def _get_uid(username):
    try:
        result = subprocess.run(
            ["id", "-u", username], capture_output=True, text=True, check=True
        )
        return int(result.stdout.strip())
    except (subprocess.CalledProcessError, ValueError):
        return 0


def _record_launch(**payload):
    try:
        with open(LAUNCH_LOG, "a") as f:
            f.write(json.dumps(payload) + "\n")
    except OSError:
        pass


def main():
    # dbus.service requires a running main loop to handle incoming method calls
    # AND to complete the async name-acquisition. Start the loop first, then
    # claim the name (calling BusName before the loop runs silently fails to
    # acquire on the GLib mainloop).
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    import threading

    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    loop = GLib.MainLoop()
    threading.Thread(target=loop.run, daemon=True).start()
    time.sleep(0.2)

    bus_name = dbus.service.BusName(BUS_NAME, bus)
    # dbus.service.BusName does NOT raise if RequestName loses to an existing
    # owner (e.g. the real helper is running). Verify we are the primary owner;
    # exit loudly so the fixture doesn't silently drive the REAL helper.
    dbus_daemon = dbus.Interface(
        bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus"),
        "org.freedesktop.DBus",
    )
    try:
        owner = str(dbus_daemon.GetNameOwner(BUS_NAME))
    except dbus.exceptions.DBusException:
        owner = ""
    if owner != bus.get_unique_name():
        print(
            f"mock helper failed to acquire {BUS_NAME} "
            f"(owner={owner!r}; is the real couchplay-helper running?)",
            file=sys.stderr,
        )
        sys.exit(1)

    # The service Object must receive the BusName (not the raw bus), else name
    # acquisition silently fails on the GLib mainloop.
    helper = MockHelper(bus_name)

    print(f"Mock helper running on {BUS_NAME} at {OBJECT_PATH}", file=sys.stderr)
    sys.stdout.flush()

    stop = threading.Event()
    import signal

    def handle_signal(signum, frame):
        stop.set()

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    stop.wait()
    helper.cleanup()  # delete any user accounts CreateUser made, so none leak
    loop.quit()


if __name__ == "__main__":
    main()
