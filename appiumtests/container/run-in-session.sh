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
export COUCHPLAY_APP_ID=/src/couchplay/build/bin/couchplay
# Runner writes kwin/app/pytest artifacts here (CWD /src/couchplay is root-owned
# in the image, so redirect to a writable dir the user can read back).
export APPIUM_ARTIFACT_OUTPUT_PATH=/tmp/cp-out

selenium-webdriver-at-spi-run /opt/e2e-venv/bin/pytest "$@"
