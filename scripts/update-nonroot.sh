#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 CouchPlay Contributors
#
# CouchPlay User-space Non-Root Updater for SteamOS
#
# Rebuilds the Flatpak and updates the systemd-sysext raw image from local source.
# Runs completely in user-space without requiring root.
# Provides instructions/commands for the user/AI to apply system configurations.
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

# Determine target directories
HOME_DIR="${HOME:-/home/deck}"
RAW_IMAGE_PATH="${HOME_DIR}/.couchplay.steamos.raw"

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
    
    if ! command -v unsquashfs &>/dev/null; then
        missing_deps+=("unsquashfs")
    fi
    
    if ! command -v mksquashfs &>/dev/null; then
        missing_deps+=("mksquashfs")
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
    
    # 1. Check local built couchplay-helper binary
    local build_helper="build/bin/couchplay-helper"
    if [[ ! -f "$build_helper" ]]; then
        print_error "Local built helper binary not found at ${build_helper}."
        print_error "Please run CMake build first: cmake --build build"
        exit 1
    fi
    
    # 2. Build & Install Flatpak application locally from directory
    print_info "Ensuring KDE SDK and Platform 6.10 are installed..."
    flatpak install --user --noninteractive -y flathub org.kde.Sdk/x86_64/6.10 org.kde.Platform/x86_64/6.10
    
    print_info "Rebuilding Flatpak application locally from source..."
    # Create a temporary copy of the manifest with local directory source override
    local manifest_copy="${TEMP_DIR}/io.github.hikaps.couchplay.json"
    cp io.github.hikaps.couchplay.json "$manifest_copy"
    # Replace the git source block with a local directory source block
    sed -i 's|"type": "git"|"type": "dir"|' "$manifest_copy"
    sed -i 's|"url": "https://github.com/hikaps/couchplay.git",||' "$manifest_copy"
    sed -i 's|"branch": "main"|"path": "."|' "$manifest_copy"
    sed -i 's|"branch": "develop"|"path": "."|' "$manifest_copy"
    
    flatpak-builder --user --install --force-clean --install-deps-from=flathub "${TEMP_DIR}/build-dir" "$manifest_copy"
    
    # 3. Build a fresh system extension raw image
    print_info "Building fresh system extension raw image..."
    ./scripts/build-sysext.sh "$RAW_IMAGE_PATH" "$build_helper"
    

    
    echo ""
    print_info "=========================================================="
    print_info "CouchPlay local user-space files updated successfully!"
    print_info "=========================================================="
    echo ""
    echo "To apply the system extension update and reload the helper service,"
    echo "please run the following commands (requires root/sudo access):"
    echo ""
    echo -e "${YELLOW}  sudo systemd-sysext refresh${NC}"
    echo -e "${YELLOW}  sudo systemctl daemon-reload${NC}"
    echo -e "${YELLOW}  sudo systemctl restart couchplay-helper.service${NC}"
    echo ""
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
