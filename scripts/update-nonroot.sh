#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 CouchPlay Contributors
#
# CouchPlay Local Flatpak Updater (SteamOS and other distros)
#
# Rebuilds and installs the Flatpak from the local source tree.
# Runs completely in user-space: no root, no sysext, no privileged commands.
#
# Usage:
#   ./scripts/update-nonroot.sh
#

set -e

# =============================================================================
# Configuration
# =============================================================================

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# (no host paths needed)

# =============================================================================
# Output Functions
# =============================================================================

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# =============================================================================
# Pre-flight Checks
# =============================================================================

if [[ $EUID -eq 0 ]]; then
    print_warn "Running as root. This script is designed to run without root privileges."
fi

check_dependencies() {
    local missing_deps=()
    
    if ! command -v flatpak &>/dev/null; then
        missing_deps+=("flatpak")
    fi
    
    if ! command -v flatpak-builder &>/dev/null; then
        missing_deps+=("flatpak-builder")
    fi
    

    if ! command -v jq &>/dev/null; then
        missing_deps+=("jq")
    fi
    
    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        print_error "Missing required dependencies for local update: ${missing_deps[*]}"
        exit 1
    fi
    
    # Check for architecture
    local arch
    arch=$(uname -m)
    if [[ "$arch" != "x86_64" ]]; then
        print_error "Unsupported architecture: $arch. CouchPlay is x86_64 only."
        exit 1
    fi
}

# =============================================================================
# Main
# =============================================================================

main() {
    print_info "CouchPlay SteamOS Local User-space Updater"
    echo ""
    
    check_dependencies
    
    # Setup temporary directory and cleanup trap
    TEMP_DIR=$(mktemp -d)
    cleanup() {
        rm -rf "$TEMP_DIR"
    }
    trap cleanup EXIT
    
    # Build & install the Flatpak locally from the working tree (bundles couchplay-padfwd / couchplay-vdisplay)
    print_info "Ensuring KDE SDK and Platform 6.10 are installed..."
    flatpak install --user --noninteractive -y flathub org.kde.Sdk/x86_64/6.10 org.kde.Platform/x86_64/6.10

    print_info "Rebuilding Flatpak application locally from source..."
    # Rewrite the couchplay git source to this checkout (jq targets it by URL, leaving other sources untouched).
    # The path must be absolute: a relative one resolves against the manifest copy in the temp directory.
    local manifest_copy="${TEMP_DIR}/io.github.hikaps.couchplay.json"
    jq --arg src "$PWD" '(.. | objects | select(.type? == "git" and ((.url? // "") | test("hikaps/couchplay")))) |= {"type": "dir", "path": $src}' \
        io.github.hikaps.couchplay.json > "$manifest_copy"

    flatpak-builder --user --install --force-clean --install-deps-from=flathub "${TEMP_DIR}/build-dir" "$manifest_copy"
    
    echo ""
    print_info "=========================================================="
    print_info "CouchPlay Flatpak rebuilt and installed successfully!"
    print_info "=========================================================="
    echo ""
    echo "Run it with:  flatpak run io.github.hikaps.couchplay"
    echo ""
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
