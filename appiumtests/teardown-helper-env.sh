#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Teardown for the helper-tier environment (inverse of setup-helper-env.sh).
# Removes the permissive test D-Bus policy, deletes the player2/player3 users,
# and stops the container D-Bus system bus that setup started. Run after the
# helper-tier tests to leave no remnants.
set -uo pipefail

POLICY=/usr/share/dbus-1/system.d/io.github.hikaps.CouchPlayHelper-test.conf

echo "== removing test D-Bus policy =="
sudo rm -f "$POLICY"
sudo dbus-send --system --dest=org.freedesktop.DBus --type=method_call \
    /org/freedesktop/DBus org.freedesktop.DBus.ReloadConfig 2>/dev/null || true

echo "== removing per-user PipeWire audio config + deleting test users =="
for u in player2 player3; do
    home=$(getent passwd "$u" | cut -d: -f6) || true
    [ -n "$home" ] && sudo rm -f "$home/.config/pipewire/pipewire-pulse.conf.d/50-couchplay.conf" 2>/dev/null || true
    sudo userdel -r "$u" 2>/dev/null || true  # -r also removes the home dir (and any remaining config)
done
sudo groupdel couchplay 2>/dev/null || true

echo "== stopping the D-Bus system bus this setup started (PID-pinned, never pkill) =="
DBUS_PIDFILE=/tmp/couchplay-test-dbus.pid
if [ -f "$DBUS_PIDFILE" ]; then
    pid=$(cat "$DBUS_PIDFILE" 2>/dev/null || true)
    if [ -n "$pid" ]; then
        sudo kill "$pid" 2>/dev/null || true
    fi
    sudo rm -f "$DBUS_PIDFILE" /run/dbus/system_bus_socket /run/dbus/pid
else
    echo "  (no pidfile: setup did not start a bus; leaving any existing bus alone)"
fi

echo "helper-tier environment torn down (no remnants)."
