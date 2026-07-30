#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 CouchPlay Contributors
#
# Packages local build binaries into the systemd-sysext image (~/.couchplay.steamos.raw)
# and refreshes the system extension layer.
#
# Usage:
#   ./scripts/update-sysext.sh
#

set -euo pipefail

RAW_IMAGE="/home/deck/.couchplay.steamos.raw"
BUILD_DIR="build"

# Verify built binaries exist
if [[ ! -f "$BUILD_DIR/bin/couchplay" ]] || [[ ! -f "$BUILD_DIR/bin/couchplay-helper" ]]; then
    echo "Error: Local build binaries not found in $BUILD_DIR/bin/."
    echo "Please compile the project first (e.g. cmake --build build)."
    exit 1
fi

# Verify systemd-sysext image exists
if [[ ! -f "$RAW_IMAGE" ]]; then
    echo "Error: Squashfs image not found at $RAW_IMAGE"
    echo "Make sure the CouchPlay SteamOS extension has been installed first."
    exit 1
fi

echo "Updating systemd-sysext raw image..."

# Setup temporary directory for unsquashing
TEMP_DIR=$(mktemp -d)
cleanup() {
    rm -rf "$TEMP_DIR"
}
trap cleanup EXIT

# 1. Unsquash the existing image to preserve libraries and metadata
echo "Unpacking $RAW_IMAGE..."
unsquashfs -d "$TEMP_DIR/mount" "$RAW_IMAGE"

# 2. Copy the newly built binaries into the unpacked structure
echo "Injecting updated binaries..."
cp "$BUILD_DIR/bin/couchplay" "$TEMP_DIR/mount/usr/local/bin/couchplay"
cp "$BUILD_DIR/bin/couchplay-helper" "$TEMP_DIR/mount/usr/local/libexec/couchplay-helper"

# 3. Copy other configuration/helper scripts if they have changed
if [[ -f "scripts/couchplay-gamemode.sh" ]]; then
    cp scripts/couchplay-gamemode.sh "$TEMP_DIR/mount/usr/local/bin/couchplay-gamemode"
    chmod +x "$TEMP_DIR/mount/usr/local/bin/couchplay-gamemode"
fi

if [[ -f "data/dbus/couchplay-helper.service" ]]; then
    cp data/dbus/couchplay-helper.service "$TEMP_DIR/mount/usr/lib/systemd/system/couchplay-helper.service"
fi

if [[ -f "data/dbus/io.github.hikaps.CouchPlayHelper.conf" ]]; then
    cp data/dbus/io.github.hikaps.CouchPlayHelper.conf "$TEMP_DIR/mount/usr/share/dbus-1/system.d/io.github.hikaps.CouchPlayHelper.conf"
fi

if [[ -f "data/dbus/io.github.hikaps.CouchPlayHelper.service" ]]; then
    cp data/dbus/io.github.hikaps.CouchPlayHelper.service "$TEMP_DIR/mount/usr/share/dbus-1/system-services/io.github.hikaps.CouchPlayHelper.service"
fi

# 4. Pack the unpacked structure back into the Squashfs image
echo "Packing raw image..."
rm -f "$RAW_IMAGE"
mksquashfs "$TEMP_DIR/mount" "$RAW_IMAGE" -noappend -all-root

# 5. Refresh systemd-sysext and restart the helper daemon
echo "Refreshing system extension layer (requires sudo)..."
sudo systemd-sysext refresh
sudo systemctl daemon-reload
sudo systemctl restart couchplay-helper.service

echo "Done! Raw image updated at $RAW_IMAGE and services refreshed."
