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

echo "== deleting player2/player3 users =="
for u in player2 player3; do
    sudo userdel -r "$u" 2>/dev/null || true
done
sudo groupdel couchplay 2>/dev/null || true

echo "== stopping the container D-Bus system bus =="
# Only the bus setup started (the host's is not visible inside the container).
# The script's own cmdline does not contain this pattern, so it is not self-matched.
sudo pkill -f 'dbus-daemon --system' 2>/dev/null || true
sudo rm -f /run/dbus/system_bus_socket /run/dbus/pid

echo "helper-tier environment torn down (no remnants)."
