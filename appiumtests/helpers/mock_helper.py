# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import subprocess
import sys
import time
from pathlib import Path

import dbus
import dbus.service

BUS_NAME = "io.github.hikaps.CouchPlayHelper"
OBJECT_PATH = "/io/github/hikaps/CouchPlayHelper"
INTERFACE_NAME = "io.github.hikaps.CouchPlayHelper"

MOCK_VERSION = "0.2.0-test"

TEST_USERS = ["player2", "player3"]
BASE_UID = 2000


class MockHelper(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, OBJECT_PATH)
        self._next_pid = 10000
        self._launched_pids = set()
        self._created_users = {}

    @dbus.service.method(INTERFACE_NAME, out_signature="s")
    def Version(self):
        return MOCK_VERSION

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="u")
    def CreateUser(self, username, fullname):
        try:
            subprocess.run(
                ["groupadd", "-f", "couchplay"], check=False, capture_output=True
            )
            result = subprocess.run(
                ["useradd", "-m", "-G", "couchplay", "-c", fullname, username],
                check=True,
                capture_output=True,
            )
            subprocess.run(
                ["loginctl", "enable-linger", username],
                check=False,
                capture_output=True,
            )
            uid = self._get_uid(username)
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

    @dbus.service.method(
        INTERFACE_NAME,
        in_signature="suassas",
        out_signature="x",
    )
    def LaunchInstance(
        self, username, compositorUid, gamescopeArgs, gameCommand, environment
    ):
        pid = self._next_pid
        self._next_pid += 1
        self._launched_pids.add(pid)
        return pid

    @dbus.service.method(INTERFACE_NAME, in_signature="x", out_signature="b")
    def StopInstance(self, pid):
        self._launched_pids.discard(pid)
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="x", out_signature="b")
    def KillInstance(self, pid):
        self._launched_pids.discard(pid)
        return True

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

    @dbus.service.method(INTERFACE_NAME, in_signature="sssasu", out_signature="b")
    def SetupOverlayMount(
        self, username, gamePath, gameId, overrideFiles, compositorUid
    ):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="b")
    def TeardownOverlayMount(self, username, gameId):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="s", out_signature="b")
    def TeardownAllUserOverlays(self, username):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="sssay", out_signature="b")
    def WriteOverrideFile(self, username, gameId, relativePath, content):
        return True

    @dbus.service.method(INTERFACE_NAME, in_signature="ss", out_signature="s")
    def GetOverlayMountPoint(self, username, gameId):
        return f"/tmp/couchplay-overlay/{username}/{gameId}"

    @dbus.service.method(INTERFACE_NAME, in_signature="ayss", out_signature="b")
    def WriteFileToUser(self, content, targetPath, username):
        return True

    def cleanup(self):
        for username in list(self._created_users.keys()):
            self.DeleteUser(username, True)


def _get_uid(self, username):
    try:
        result = subprocess.run(
            ["id", "-u", username], capture_output=True, text=True, check=True
        )
        return int(result.stdout.strip())
    except (subprocess.CalledProcessError, ValueError):
        return 0


MockHelper._get_uid = _get_uid


def main():
    bus = dbus.SystemBus()
    bus_name = dbus.service.BusName(BUS_NAME, bus)

    helper = MockHelper(bus)

    print(f"Mock helper running on {BUS_NAME} at {OBJECT_PATH}", file=sys.stderr)
    sys.stdout.flush()

    import signal

    def handle_signal(signum, frame):
        helper.cleanup()
        sys.exit(0)

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        helper.cleanup()


if __name__ == "__main__":
    main()
