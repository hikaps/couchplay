#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Bundle the runtime shared libraries of an ELF binary next to it and set an
# $ORIGIN-relative RPATH so it runs without system copies of those libs.
#
# Used for the privileged helper so it starts on minimal distros (e.g. Arch with
# no system Qt6/Polkit). glibc and the dynamic linker are deliberately NOT bundled
# — they must stay system-provided, which is why the binary still has a glibc floor
# (see the build container in the release workflow).
#
# Usage: bundle-libs.sh <binary> [librel]
#   binary : ELF binary to process (in place)
#   librel : lib dir relative to $ORIGIN/.. (default: lib/couchplay)
# Requires: patchelf
set -euo pipefail

bin="${1:?usage: bundle-libs.sh <binary> [librel]}"
librel="${2:-lib/couchplay}"

command -v patchelf >/dev/null 2>&1 || { echo "patchelf not found" >&2; exit 1; }

bindir="$(cd "$(dirname "$bin")" && pwd)"
libdir="$bindir/../$librel"
rm -rf "$libdir"
mkdir -p "$libdir"

# glibc core + dynamic linker must stay system-provided (bundling them breaks the
# loader on other systems). Keep libstdc++/libgcc_s (C++ ABI portability).
exclude='^(libc|ld-linux|libdl|libm|libpthread|librt|libresolv|libutil|libcrypt|libBrokenLocale|libanl)\.so'

# ldd prints resolved deps as "<soname> => <abspath> (0x...)". Copy each one
# (following symlinks so the file is named by its SONAME) except the glibc family.
ldd "$bin" 2>/dev/null | awk '/=> \// {print $3}' | sort -u | while IFS= read -r lib; do
    [[ -n "$lib" ]] || continue
    base="$(basename "$lib")"
    if [[ "$base" =~ $exclude ]]; then
        continue
    fi
    cp -L "$lib" "$libdir/"
done

# Use --force-rpath so the path is DT_RPATH, not DT_RUNPATH. DT_RPATH applies to the
# whole dependency tree; DT_RUNPATH only resolves the binary's *direct* deps, so the
# bundled transitive libs (e.g. Qt6Core's libpcre2/libdouble-conversion/libzstd) would
# be unreachable on minimal systems and the helper would still fail to load (#28).
# Path is relative to the binary's own location ($ORIGIN = its directory).
patchelf --force-rpath --set-rpath "\$ORIGIN/../$librel" "$bin" >/dev/null

count=$(find "$libdir" -maxdepth 1 -type f | wc -l)
echo "Bundled ${count} library(ies) for $(basename "$bin") -> ${libdir} (RPATH \$ORIGIN/../${librel})"
