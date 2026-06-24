#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Stub "gamescope": opens a window whose Wayland app_id / WM_CLASS is "gamescope"
# so the app's WindowManager (which matches windows by resourceClass ==
# "gamescope") can find and position it. Stays open until killed. Anything after
# `--` (the game command) is ignored.
#
# Used by mock_helper.LaunchInstance when COUCHPLAY_STUB_GAMESCOPE points here.
#
# NOTE: we deliberately do NOT use Gtk.Application — its application_id must be
# reverse-DNS, which would not yield resourceClass "gamescope". Instead we set
# the program name + WM_CLASS so GTK's Wayland backend reports app_id "gamescope".
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk

GLib.set_prgname("gamescope")

win = Gtk.Window(title="gamescope")
win.set_wmclass("gamescope", "gamescope")
win.set_default_size(512, 768)
win.connect("destroy", Gtk.main_quit)
win.show_all()

Gtk.main()
