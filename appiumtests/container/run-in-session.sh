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
# Software rendering: rootless distrobox can't access /dev/dri (EACCES on
# amdgpu). AT-SPI queries the a11y tree, not pixels, so llvmpipe + the Qt Quick
# software backend are sufficient and avoid any GPU dependency.
export LIBGL_ALWAYS_SOFTWARE=1
export QT_QUICK_BACKEND=software
export TEST_WITH_VIDEO_RECORDER=0
# The AT-SPI driver launches this executable and tracks its PID. Keep exec so
# the observed process is CouchPlay, while preserving its Qt warnings in CI.
export APPIUM_ARTIFACT_OUTPUT_PATH=/tmp/cp-out
mkdir -p "$APPIUM_ARTIFACT_OUTPUT_PATH"
export COUCHPLAY_APP_ID="$APPIUM_ARTIFACT_OUTPUT_PATH/couchplay-launch"
cat > "$COUCHPLAY_APP_ID" <<'EOF'
#!/bin/sh
exec /src/couchplay/build/bin/couchplay "$@" >>/tmp/cp-out/couchplay-app.log 2>&1
EOF
chmod 700 "$COUCHPLAY_APP_ID"

selenium-webdriver-at-spi-run /opt/e2e-venv/bin/pytest "$@"
