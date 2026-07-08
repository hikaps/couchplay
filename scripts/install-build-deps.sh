#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Install CouchPlay's Fedora build dependencies.
#
# Single source of truth for the dnf package list so it can't drift across the
# ci / beta / release / arch-smoke workflows (the divergence class that bit
# #28/#37). The core list lives here; each caller passes only the few extra
# packages it uniquely needs:
#   ci:          install-build-deps.sh dbus-daemon
#   beta/release: install-build-deps.sh tar patchelf
#   arch-smoke:  install-build-deps.sh patchelf binutils
#
# Run on Fedora (provides dnf). Note: the build workflows install git before
# `actions/checkout` (the bare fedora:41 image lacks it); git is also in the
# core list below, so the second install is an idempotent no-op there.
set -euo pipefail

dnf install -y \
  cmake gcc-c++ git make \
  qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qt5compat-devel \
  kf6-kirigami-devel kf6-ki18n-devel kf6-kcoreaddons-devel \
  kf6-kconfig-devel kf6-kiconthemes-devel kf6-qqc2-desktop-style \
  kf6-kglobalaccel-devel extra-cmake-modules \
  pipewire-devel polkit-devel polkit-qt6-1-devel \
  "$@"
