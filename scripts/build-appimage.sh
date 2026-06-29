#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Build a portable CouchPlay AppImage.
#
# The GUI (couchplay) is bundled with its Qt6/KF6 runtime libraries so it runs on
# any glibc >= build-baseline desktop without system Qt6/KF6. The privileged
# helper cannot run from the AppImage mount, so it is carried inside and installed
# on demand via `./CouchPlay-x86_64.AppImage install-helper`.
#
# Build baseline = the container this runs in. In CI we use fedora:40 (glibc 2.39)
# so the AppImage runs on Arch/Fedora/Ubuntu 24.04 LTS/Debian 13 and newer. To
# widen compatibility, lower the base (e.g. an older distro) — but KF6 must be
# available there.
#
# Usage: ./scripts/build-appimage.sh
# Env:   BUILD_DIR, APPDIR, ARCH, TOOLS_DIR, OUTPUT (all optional)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
APPDIR="${APPDIR:-AppDir}"
ARCH="${ARCH:-$(uname -m)}"
OUTPUT="${OUTPUT:-CouchPlay-${ARCH}.AppImage}"
TOOLS_DIR="${TOOLS_DIR:-${ROOT}/.appimage-tools}"

# AppImage tools may need to run where FUSE is unavailable (CI containers).
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP=1

echo "==> Building CouchPlay (${ARCH})"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> Preparing AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/libexec" "$APPDIR/usr/share/couchplay/data"

# GUI binary is placed/bundled by linuxdeploy below.
# Helper + installer + privileged data are carried verbatim (no bundling: the
# helper runs from system paths after `install-helper` and uses system Qt6).
cp "$BUILD_DIR/bin/couchplay-helper" "$APPDIR/usr/libexec/"
cp scripts/install-helper.sh "$APPDIR/usr/share/couchplay/"
cp -r data/dbus data/polkit data/pipewire "$APPDIR/usr/share/couchplay/data/"

echo "==> Fetching linuxdeploy / Qt plugin / appimagetool"
mkdir -p "$TOOLS_DIR"
fetch() { # <url> <dest>
    if [[ ! -x "$2" ]]; then
        curl -fsSL "$1" -o "$2"
        chmod +x "$2"
    fi
}
LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-${ARCH}.AppImage"
QT_PLUGIN="$TOOLS_DIR/linuxdeploy-plugin-qt-${ARCH}.AppImage"
APPIMAGETOOL="$TOOLS_DIR/appimagetool-${ARCH}.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/latest/download/linuxdeploy-${ARCH}.AppImage" "$LINUXDEPLOY"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/latest/download/linuxdeploy-plugin-qt-${ARCH}.AppImage" "$QT_PLUGIN"
fetch "https://github.com/AppImage/AppImageKit/releases/latest/download/appimagetool-${ARCH}.AppImage" "$APPIMAGETOOL"

echo "==> Bundling GUI + Qt6/KF6 runtime (linuxdeploy + Qt plugin)"
# Only `couchplay` is bundled (the GUI). --custom-apprun installs our wrapper that
# supports the `install-helper` subcommand and sets Qt library/plugin paths.
"$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$BUILD_DIR/bin/couchplay" \
    --desktop-file data/io.github.hikaps.couchplay.desktop \
    --icon-file data/icons/io.github.hikaps.couchplay.png \
    --custom-apprun "$ROOT/scripts/AppRun" \
    --plugin qt

echo "==> Packaging AppImage"
rm -f "$OUTPUT"
ARCH="$ARCH" "$APPIMAGETOOL" "$APPDIR" "$OUTPUT"

echo "==> Built $OUTPUT"
