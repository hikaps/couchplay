#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 CouchPlay Contributors
#
# CouchPlay System Extension (sysext) Image Builder
#
# Builds a fresh helper-only systemd-sysext Squashfs raw image from scratch.
#
# Usage:
#   ./scripts/build-sysext.sh <output_image_path> <helper_binary_path>
#
# Requirements:
#   - squashfs-tools (mksquashfs)
#   - patchelf (for bundling libs)
#

set -euo pipefail

# Output functions
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_info() {
    echo -e "${GREEN}[build-sysext]${NC} $1"
}

print_error() {
    echo -e "${RED}[build-sysext ERROR]${NC} $1"
}

# Parse arguments
if [[ $# -lt 2 ]]; then
    print_error "Usage: $0 <output_image_path> <helper_binary_path>"
    exit 1
fi

OUTPUT_IMAGE=$(readlink -f "$1" || echo "$1")
HELPER_BIN=$(readlink -f "$2" || echo "$2")

# Check helper binary exists
if [[ ! -f "$HELPER_BIN" ]]; then
    print_error "Helper binary not found at: $HELPER_BIN"
    exit 1
fi

# Check requirements
if ! command -v mksquashfs &>/dev/null; then
    print_error "mksquashfs is required to build the sysext image. Install squashfs-tools."
    exit 1
fi

# Determine source directories relative to this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Setup temporary staging directory
STAGE_DIR=$(mktemp -d)
cleanup() {
    rm -rf "$STAGE_DIR"
}
trap cleanup EXIT

print_info "Creating systemd-sysext staging structure..."
mkdir -p "$STAGE_DIR/usr/local/libexec"
mkdir -p "$STAGE_DIR/usr/share/dbus-1/system.d"
mkdir -p "$STAGE_DIR/usr/share/dbus-1/system-services"
mkdir -p "$STAGE_DIR/usr/lib/systemd/system"
mkdir -p "$STAGE_DIR/usr/share/polkit-1/actions"
mkdir -p "$STAGE_DIR/usr/share/pipewire/pipewire-pulse.conf.d"
mkdir -p "$STAGE_DIR/usr/lib/extension-release.d"

print_info "Copying helper binary..."
cp "$HELPER_BIN" "$STAGE_DIR/usr/local/libexec/couchplay-helper"

# Bundle helper's runtime libraries (RPATH is $ORIGIN/../lib/couchplay)
print_info "Bundling runtime libraries..."
(cd "$PROJECT_DIR" && ./scripts/package-helper.sh "$STAGE_DIR/usr/local/libexec/couchplay-helper")

print_info "Copying configuration files..."
# D-Bus configuration
if [[ -f "$PROJECT_DIR/data/dbus/io.github.hikaps.CouchPlayHelper.conf" ]]; then
    cp "$PROJECT_DIR/data/dbus/io.github.hikaps.CouchPlayHelper.conf" \
        "$STAGE_DIR/usr/share/dbus-1/system.d/io.github.hikaps.CouchPlayHelper.conf"
fi
if [[ -f "$PROJECT_DIR/data/dbus/io.github.hikaps.CouchPlayHelper.service" ]]; then
    cp "$PROJECT_DIR/data/dbus/io.github.hikaps.CouchPlayHelper.service" \
        "$STAGE_DIR/usr/share/dbus-1/system-services/io.github.hikaps.CouchPlayHelper.service"
fi

# Systemd service
if [[ -f "$PROJECT_DIR/data/dbus/couchplay-helper.service" ]]; then
    cp "$PROJECT_DIR/data/dbus/couchplay-helper.service" \
        "$STAGE_DIR/usr/lib/systemd/system/couchplay-helper.service"
fi

# Polkit policy
if [[ -f "$PROJECT_DIR/data/polkit/io.github.hikaps.couchplay.policy" ]]; then
    cp "$PROJECT_DIR/data/polkit/io.github.hikaps.couchplay.policy" \
        "$STAGE_DIR/usr/share/polkit-1/actions/io.github.hikaps.couchplay.policy"
fi

# PipeWire PulseAudio listener configuration
if [[ -f "$PROJECT_DIR/data/pipewire/50-couchplay.conf" ]]; then
    cp "$PROJECT_DIR/data/pipewire/50-couchplay.conf" \
        "$STAGE_DIR/usr/share/pipewire/pipewire-pulse.conf.d/50-couchplay.conf"
fi

# Create systemd-sysext extension-release file
print_info "Writing extension-release metadata..."
echo "ID=_any" > "$STAGE_DIR/usr/lib/extension-release.d/extension-release.couchplay.steamos"

# Build squashfs raw image
print_info "Building Squashfs raw image at $OUTPUT_IMAGE..."
rm -f "$OUTPUT_IMAGE"
mksquashfs "$STAGE_DIR" "$OUTPUT_IMAGE" -noappend -all-root

print_info "Sysext image build completed successfully!"
