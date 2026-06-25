#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Runs inside the dbus-run-session (isolated session bus): starts a nested
# kwin_wayland (registers org.kde.KWin on THIS bus) and the selenium runner.
# The app launched by the runner shares this bus, so its WindowManager queries
# the nested kwin -- fixing the compositor/session-bus mismatch.
set -e

kwin_wayland --no-lockscreen --no-global-shortcuts --locale1 >/tmp/kwin.log 2>&1 &
KWIN_PID=$!

# wait for the nested wayland socket
for _ in $(seq 1 40); do
    [ -S "${XDG_RUNTIME_DIR:-/run/user/0}/wayland-1" ] && break
    sleep 0.3
done
export WAYLAND_DISPLAY=wayland-1

# run the appium tests; the runner launches the app, which sees the mock (system
# bus) + the nested kwin (this session bus).
selenium-webdriver-at-spi-run /opt/e2e-venv/bin/pytest "$@"
RC=$?

kill "$KWIN_PID" 2>/dev/null || true
exit $RC
