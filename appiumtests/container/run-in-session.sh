#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Runs inside the dbus-run-session (isolated session bus). The selenium runner
# starts its OWN nested kwin_wayland (registers org.kde.KWin on THIS bus), so we
# must NOT start one ourselves. The app launched by the runner shares this bus,
# so its WindowManager queries the nested kwin -- fixing the compositor mismatch.
set -e

export QT_QPA_PLATFORM=wayland
export TEST_WITH_VIDEO_RECORDER=0
export COUCHPLAY_APP_ID=/src/couchplay/build/bin/couchplay

selenium-webdriver-at-spi-run /opt/e2e-venv/bin/pytest "$@"
