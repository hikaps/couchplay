#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Stub "gamescope": opens a window whose Wayland app_id is "gamescope" so the
# app's WindowManager (which matches windows by resourceClass == "gamescope")
# can find and position it. Stays open until killed. Args after `--` (the game
# command) are ignored.
#
# Used by mock_helper.LaunchInstance when COUCHPLAY_STUB_GAMESCOPE points here.
#
# Qt (not GTK): GTK3 cannot set a Wayland app_id of "gamescope" (Gtk.Application
# requires a reverse-DNS id). Qt6 derives the Wayland app_id from the
# application name, so setApplicationName("gamescope") -> resourceClass "gamescope".
import sys

from PySide6 import QtWidgets
# Qt6 derives the Wayland app_id from the application name, read at platform
# init; setting argv[0] (the executable-name fallback) to "gamescope" before
# constructing QApplication makes the app_id / resourceClass "gamescope".
sys.argv[0] = "gamescope"

app = QtWidgets.QApplication(sys.argv)
app.setApplicationName("gamescope")
app.setApplicationDisplayName("gamescope")
win = QtWidgets.QWidget()
win.setWindowTitle("gamescope")
win.resize(512, 768)
win.show()

sys.exit(app.exec())
