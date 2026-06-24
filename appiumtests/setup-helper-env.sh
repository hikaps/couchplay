#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Provision the system-bus mock-helper environment for the e2e helper-tier tests.
# TEST-ONLY. Run once (sudo) before: selenium-webdriver-at-spi-run pytest appiumtests/test_session.py
#
# 1. Installs a permissive D-Bus policy so a user-run mock helper can own
#    io.github.hikaps.CouchPlayHelper AND any user can call it (the real policy
#    restricts ownership to root and calls to wheel/games).
# 2. Pre-creates the player2/player3 gaming users the session tests select.
set -euo pipefail

echo "== ensuring a responding D-Bus system bus =="
if ! dbus-send --system --dest=org.freedesktop.DBus --print-reply \
        /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1; then
    # stale socket file may linger after a killed daemon -> remove and restart
    sudo rm -f /run/dbus/system_bus_socket /run/dbus/pid
    sudo mkdir -p /run/dbus
    sudo dbus-daemon --system --fork
fi

POLICY=/usr/share/dbus-1/system.d/io.github.hikaps.CouchPlayHelper-test.conf

echo "== installing permissive test D-Bus policy =="
sudo tee "$POLICY" >/dev/null <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy context="default">
    <allow own="io.github.hikaps.CouchPlayHelper"/>
    <allow send_destination="io.github.hikaps.CouchPlayHelper"/>
    <allow receive_sender="io.github.hikaps.CouchPlayHelper"/>
  </policy>
</busconfig>
EOF
sudo dbus-send --system --dest=org.freedesktop.DBus --type=method_call \
    /org/freedesktop/DBus org.freedesktop.DBus.ReloadConfig || true

echo "== pre-creating player2/player3 gaming users =="
sudo groupadd -f couchplay
for u in player2 player3; do
    if ! id "$u" >/dev/null 2>&1; then
        sudo useradd -m -G couchplay "$u"
    fi
    sudo loginctl enable-linger "$u" 2>/dev/null || true
done

echo "helper-tier environment ready."
