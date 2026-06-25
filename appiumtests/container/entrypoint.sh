#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Rootless container entrypoint. Runs entirely as the (non-root) container user:
#   - a user-owned "system" bus: a permissive --session dbus-daemon whose
#     address we export as DBUS_SYSTEM_BUS_ADDRESS. The app's
#     QDBusConnection::systemBus() and the mock's dbus.SystemBus() both honor
#     it, so NO real system bus, NO root, and NO polkit/bus policy is needed;
#   - the mock helper (owns io.github.hikaps.CouchPlayHelper there, fake-users
#     mode -> returns plausible uids without useradd, so no root, no leak);
#   - headless PipeWire + WirePlumber.
# Then the test command runs under an isolated dbus-run-session so the selenium
# runner's NESTED kwin registers org.kde.KWin on that bus -- the one the app's
# WindowManager queries. Test args pass through to pytest.
set -e

# Scrub host session/display env that distrobox leaks in, then use a private
# runtime dir. The host owns /run/user/<uid>/wayland-0 (its compositor) and its
# own AT-SPI/DBus session paths; leaking them makes the nested kwin/portal
# collide with the host. A clean slate + a private dir lets kwin run headless
# (AT-SPI needs no pixels) and lets at-spi spin up its own bus.
unset WAYLAND_DISPLAY DISPLAY AT_SPI_BUS_ADDRESS DBUS_SESSION_BUS_ADDRESS XAUTHORITY
export XDG_RUNTIME_DIR="/tmp/cp-runtime"
mkdir -p "$XDG_RUNTIME_DIR"

echo "[entrypoint] starting user-owned system bus + mock helper + PipeWire"
# --session is permissive (any name ownable); reuse it as the "system" bus.
# --fork backgrounds the daemon; --print-address=1 prints its address to stdout.
export DBUS_SYSTEM_BUS_ADDRESS="$(dbus-daemon --session --print-address=1 --fork)"
export COUCHPLAY_MOCK_EXTERNAL=1   # conftest: mock already up, do not start one
export COUCHPLAY_MOCK_FAKE_USERS=1 # mock: fake uids, no useradd (no root)

MOCK=/src/couchplay/appiumtests/helpers/mock_helper.py
python3 "$MOCK" >/tmp/mock-helper.log 2>&1 &

pipewire >/tmp/pipewire.log 2>&1 &
sleep 1
wireplumber >/tmp/wireplumber.log 2>&1 &

# wait for the mock to own the helper name on the user-owned system bus
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
