#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Bundle the helper's runtime libraries and assert the bundle landed.
#
# Shared by the beta/release publish workflows and the arch-smoke test so the
# packaging sequence has a single source of truth and can't silently drift
# between them -- the divergence that let beta ship without bundling (#28/#37).
#
# Usage: package-helper.sh <binary> [librel]
#   binary : helper ELF to bundle (processed in place)
#   librel : lib dir relative to $ORIGIN/.. (default: lib/couchplay); passed
#            through to bundle-libs.sh
# Requires: patchelf, and bundle-libs.sh as a sibling of this script.
set -euo pipefail

bin="${1:?usage: package-helper.sh <binary> [librel]}"
librel="${2:-lib/couchplay}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bundle="$here/bundle-libs.sh"
chmod +x "$bundle"

# Bundle the runtime libs: ships the build-env Qt6Core next to the binary so the
# helper doesn't fall back to a system Qt6Core on Arch/SteamOS.
"$bundle" "$bin" "$librel"

# Regression guard (#28/#37): the bundle must land and the helper must point at
# it, else the binary loads a system libQt6Core.so.6 that glibc rejects via
# GNU_PROPERTY_1_NEEDED_INDIRECT_EXTERN_ACCESS (service exit 127).
libdir="$(dirname "$bin")/../$librel"
if [[ ! -f "$libdir/libQt6Core.so.6" ]]; then
    echo "::error::bundled libQt6Core.so.6 missing under $libdir"
    exit 1
fi
rpath="$(patchelf --print-rpath "$bin")"
if [[ "$rpath" != *"$librel"* ]]; then
    echo "::error::helper RPATH '$rpath' does not reference the bundled libs ($librel)"
    exit 1
fi
echo "[package-helper] OK: libQt6Core.so.6 bundled, RPATH=$rpath"
