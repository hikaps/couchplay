#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Container entrypoint: starts the container-private system bus, the mock helper
# (owns io.github.hikaps.CouchPlayHelper), and the headless audio graph; waits
# for the mock to be ready; then runs the test command under an isolated session
# bus (dbus-run-session) so the nested kwin's org.kde.KWin is the one the app's
# WindowManager queries. Test args are passed through to pytest.
set -e

echo "[entrypoint] starting system bus + mock helper + PipeWire"
mkdir -p /run/dbus
dbus-daemon --system --fork

python3 /src/couchplay/appiumtests/helpers/mock_helper.py &

pipewire >/tmp/pipewire.log 2>&1 &
sleep 1
wireplumber >/tmp/wireplumber.log 2>&1 &

# wait for the mock to own the helper name
for _ in $(seq 1 20); do
    if dbus-send --system --dest=org.freedesktop.DBus --print-reply \
            /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner \
            string:io.github.hikaps.CouchPlayHelper 2>/dev/null | grep -q true; then
        echo "[entrypoint] mock helper ready"
        break
    fi
    sleep 1
done

export QT_QPA_PLATFORM=wayland
export TEST_WITH_VIDEO_RECORDER=0
export COUCHPLAY_APP_ID=/src/couchplay/build/bin/couchplay

echo "[entrypoint] running tests under isolated session bus (nested kwin)"
exec dbus-run-session /src/couchplay/appiumtests/container/run-in-session.sh "$@"
