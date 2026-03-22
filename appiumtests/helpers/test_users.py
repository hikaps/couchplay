# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import subprocess

TEST_USERS = ["player2", "player3"]


def create_test_users():
    subprocess.run(["groupadd", "-f", "couchplay"], check=False)
    for username in TEST_USERS:
        subprocess.run(
            ["useradd", "-m", "-G", "couchplay", username],
            check=False,
            capture_output=True,
        )
        subprocess.run(
            ["loginctl", "enable-linger", username],
            check=False,
            capture_output=True,
        )


def remove_test_users():
    for username in TEST_USERS:
        subprocess.run(
            ["userdel", "-r", username],
            check=False,
            capture_output=True,
        )


def get_user_uid(username):
    result = subprocess.run(["id", "-u", username], capture_output=True, text=True)
    if result.returncode == 0:
        try:
            return int(result.stdout.strip())
        except ValueError:
            return 0
    return 0
