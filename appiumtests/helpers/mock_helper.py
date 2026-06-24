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

import dbus
import dbus.service

BUS_NAME = "io.github.hikaps.CouchPlayHelper"
OBJECT_PATH = "/io/github/hikaps/CouchPlayHelper"
INTERFACE_NAME = "io.github.hikaps.CouchPlayHelper"

TEST_USERS = ["player2", "player3"]
LAUNCH_LOG = os.environ.get(
    "COUCHPLAY_MOCK_LAUNCH_LOG", "/tmp/couchplay-mock-launch.jsonl"
)


class MockHelper(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, OBJECT_PATH)
        self._next_pid = 10000
        self._launched_pids = set()
        self._stub_procs = {}
        self._created_users = {}
        # Truncate any stale launch log from a previous run.
        try:
            open(LAUNCH_LOG, "w").close()
        except OSError:
            pass

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="u")
    def CreateUser(self, username, fullname):
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
        stub = os.environ.get("COUCHPLAY_STUB_GAMESCOPE")
        if stub:
            # Launch the stub gamescope so a real window exists for the app's
            # WindowManager to position; return its actual PID.
            proc = subprocess.Popen(
                [sys.executable, stub],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            pid = proc.pid
            self._stub_procs[pid] = proc
        else:
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
        pid = int(pid)
        proc = self._stub_procs.pop(pid, None)
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        self._launched_pids.discard(pid)

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

    @dbus.service.method(INTERFACE_NAME, in_signature="ayss", out_signature="b")
    def WriteFileToUser(self, content, targetPath, username):
        return True

    def cleanup(self):
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
