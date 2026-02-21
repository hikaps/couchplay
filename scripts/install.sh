#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# CouchPlay One-Liner Installer
#
# Downloads and installs CouchPlay from GitHub releases.
# This script must be run with root privileges (sudo).
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | sudo bash
#   sudo ./install.sh
#
# Requirements:
#   - curl: for downloading files
#   - tar: for extracting the release tarball
#   - sha256sum: for verifying checksums
#   - x86_64 architecture

set -e

# =============================================================================
# Configuration
# =============================================================================

REPO_OWNER="hikaps"
REPO_NAME="couchplay"
GITHUB_API="https://api.github.com"

# Installation paths (overridable via environment)
PREFIX="${PREFIX:-/usr/local}"
BIN_DIR="${BIN_DIR:-${PREFIX}/bin}"
LIBEXEC_DIR="${LIBEXEC_DIR:-${PREFIX}/libexec}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

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

check_root() {
    if [[ $EUID -ne 0 ]]; then
        print_error "This script must be run as root (use sudo)"
        echo ""
        echo "Usage:"
        echo "  curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | sudo bash"
        echo "  curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | sudo bash"
        echo "  sudo ./install.sh"
        exit 1
    fi
}

check_dependencies() {
    local missing_deps=()
    
    if ! command -v curl &>/dev/null; then
        missing_deps+=("curl")
    fi
    
    if ! command -v tar &>/dev/null; then
        missing_deps+=("tar")
    fi
    
    if ! command -v sha256sum &>/dev/null; then
        missing_deps+=("sha256sum")
    fi
    
    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        print_error "Missing required dependencies: ${missing_deps[*]}"
        echo ""
        echo "Please install the missing dependencies and try again."
        echo ""
        echo "On Debian/Ubuntu:"
        echo "  sudo apt install ${missing_deps[*]}"
        echo ""
        echo "On Fedora/RHEL:"
        echo "  sudo dnf install ${missing_deps[*]}"
        echo ""
        echo "On Arch Linux:"
        echo "  sudo pacman -S ${missing_deps[*]}"
        exit 1
    fi
}

check_architecture() {
    local arch
    arch=$(uname -m)
    
    if [[ "$arch" != "x86_64" ]]; then
        print_error "Unsupported architecture: $arch"
        echo ""
        echo "CouchPlay is currently only available for x86_64 (AMD64) systems."
        echo "Your system is running: $arch"
        echo ""
        echo "If you would like to see support for your architecture, please"
        echo "open an issue at: https://github.com/${REPO_OWNER}/${REPO_NAME}/issues"
        exit 1
    fi
    
    print_info "Architecture check passed: $arch"
}

# =============================================================================
# GitHub API Functions
# =============================================================================

get_latest_release() {
    # Fetches the latest stable release metadata from GitHub Releases API
    # The /releases/latest endpoint excludes pre-releases
    # Returns JSON with: tag_name, name, assets[], etc.
    
    local api_url="${GITHUB_API}/repos/${REPO_OWNER}/${REPO_NAME}/releases/latest"
    local response
    local http_code
    
    print_info "Fetching latest release information..."
    
    # Use curl with silent mode, but capture HTTP status code
    response=$(curl -sL -w "\n%{http_code}" "$api_url" 2>/dev/null)
    http_code=$(echo "$response" | tail -n1)
    response=$(echo "$response" | sed '$d')
    
    if [[ "$http_code" != "200" ]]; then
        print_error "Failed to fetch release information (HTTP $http_code)"
        echo ""
        echo "This could mean:"
        echo "  - No releases have been published yet"
        echo "  - GitHub API rate limit exceeded"
        echo "  - Network connectivity issues"
        echo ""
        echo "Please check: https://github.com/${REPO_OWNER}/${REPO_NAME}/releases"
        exit 1
    fi
    
    echo "$response"
}

get_release_tag() {
    # Extracts tag_name from release JSON
    echo "$1" | grep -m1 '"tag_name"' | cut -d'"' -f4
}

get_release_name() {
    # Extracts release name from release JSON
    echo "$1" | grep -m1 '"name"' | cut -d'"' -f4
}

get_asset_url() {
    # Extracts browser_download_url for a matching asset pattern
    # Usage: get_asset_url "$release_json" "couchplay-.*-linux.tar.gz"
    local release_json="$1"
    local pattern="$2"
    
    echo "$release_json" | grep -o "\"browser_download_url\": \"[^\"]*\"" | \
        grep -E "$pattern" | \
        head -1 | \
        cut -d'"' -f4
}

# =============================================================================
# Download and Install Functions
# =============================================================================

download_file() {
    # Downloads a file from URL to specified output path
    # Returns 0 on success, 1 on failure
    local url="$1"
    local output="$2"
    
    print_info "Downloading: $url"
    
    if ! curl -fsSL "$url" -o "$output"; then
        print_error "Failed to download: $url"
        return 1
    fi
    
    return 0
}

verify_checksum() {
    # Verifies the tarball checksum against the .sha256 file
    # The .sha256 file format: <hash>  <filename>
    # Uses sha256sum -c for verification
    # FAILS HARD on mismatch or missing checksum file
    local tarball="$1"
    local checksum_file="$2"
    local tarball_dir
    
    print_info "Verifying checksum..."
    
    # Check that checksum file exists
    if [[ ! -f "$checksum_file" ]]; then
        print_error "Checksum file not found: $checksum_file"
        print_error "Cannot verify tarball integrity - aborting for safety"
        exit 1
    fi
    
    # Check that tarball exists
    if [[ ! -f "$tarball" ]]; then
        print_error "Tarball not found: $tarball"
        exit 1
    fi
    
    # sha256sum -c expects to be run from the directory containing the file
    # The .sha256 file contains relative filenames
    tarball_dir=$(dirname "$tarball")
    
    # Run verification from the tarball directory
    if ! (cd "$tarball_dir" && sha256sum -c "$(basename "$checksum_file")" --strict --quiet 2>/dev/null); then
        print_error "Checksum verification FAILED!"
        print_error "The downloaded file may be corrupted or tampered with."
        print_error "Aborting installation for safety."
        exit 1
    fi
    
    print_info "Checksum verification passed"
    return 0
}

extract_tarball() {
    # Extracts the tarball to the specified directory
    local tarball="$1"
    local extract_dir="$2"
    
    print_info "Extracting tarball..."
    
    if ! tar -xJf "$tarball" -C "$extract_dir"; then
        print_error "Failed to extract tarball"
        return 1
    fi
    
    return 0
}

install_binary() {
    # Installs the main couchplay binary to BIN_DIR
    # Uses 'install' command for proper permissions
    local extract_dir="$1"
    local binary_name
    
    # The tarball extracts to a subdirectory named couchplay-x86_64 or similar
    # Find the actual extracted directory containing bin/
    local bin_dir
    bin_dir=$(find "$extract_dir" -type d -name "bin" | head -1)
    
    if [[ -z "$bin_dir" ]]; then
        print_error "Could not find bin/ directory in extracted tarball"
        return 1
    fi
    
    # Install the main binary
    print_info "Installing couchplay binary to ${BIN_DIR}"
    
    # Create BIN_DIR if it doesn't exist (idempotent)
    mkdir -p "$BIN_DIR"
    
    # Use install command for proper permissions (755)
    if ! install -Dm755 "${bin_dir}/couchplay" "${BIN_DIR}/couchplay"; then
        print_error "Failed to install couchplay binary"
        return 1
    fi
    
    print_info "Binary installed successfully"
    return 0
}

install_helper() {
    # Runs the install-helper.sh script from the extracted release directory
    # This installs the privileged helper service, D-Bus config, and polkit policy
    local extract_dir="$1"
    
    # Find the install-helper.sh script
    local helper_script
    helper_script=$(find "$extract_dir" -name "install-helper.sh" -type f | head -1)
    
    if [[ -z "$helper_script" ]]; then
        print_error "Could not find install-helper.sh in extracted tarball"
        return 1
    fi
    
    local helper_dir
    helper_dir=$(dirname "$helper_script")
    
    print_info "Installing helper service..."
    
    # Run the helper installer from its directory
    # The helper script expects to be run from its own directory
    local current_dir
    current_dir=$(pwd)
    
    cd "$helper_dir"
    
    if ! ./install-helper.sh install; then
        cd "$current_dir"
        print_error "Helper installation failed"
        return 1
    fi
    
    cd "$current_dir"
    
    print_info "Helper service installed successfully"
    return 0
}

cleanup() {
    # Cleans up the temporary directory
    # Safe to call multiple times
    if [[ -n "${TEMP_DIR:-}" && -d "$TEMP_DIR" ]]; then
        print_info "Cleaning up temporary files..."
        rm -rf "$TEMP_DIR"
    fi
}

# =============================================================================
# Main
# =============================================================================

main() {
    print_info "CouchPlay Installer"
    echo ""
    
    # Pre-flight checks
    check_root
    check_dependencies
    check_architecture
    
    echo ""
    
    # Get latest release info
    local release_json
    release_json=$(get_latest_release)
    
    local tag_name
    tag_name=$(get_release_tag "$release_json")
    
    print_info "Latest release: $tag_name"
    
    # Get asset URLs
    local tarball_url checksum_url
    tarball_url=$(get_asset_url "$release_json" "couchplay-x86_64\.tar\.xz")
    checksum_url=$(get_asset_url "$release_json" "couchplay-x86_64\.sha256")
    
    if [[ -z "$tarball_url" ]]; then
        print_error "Could not find tarball asset in release"
        exit 1
    fi
    
    if [[ -z "$checksum_url" ]]; then
        print_error "Could not find checksum asset in release"
        print_error "Refusing to install without checksum verification"
        exit 1
    fi
    
    print_info "Tarball: $tarball_url"
    print_info "Checksum: $checksum_url"
    
    # Setup temporary directory and cleanup trap
    TEMP_DIR=$(mktemp -d)
    trap cleanup EXIT
    
    local tarball_file="${TEMP_DIR}/couchplay-x86_64.tar.xz"
    local checksum_file="${TEMP_DIR}/couchplay-x86_64.sha256"
    local extract_dir="${TEMP_DIR}/extract"
    
    mkdir -p "$extract_dir"
    
    # Download files
    echo ""
    if ! download_file "$tarball_url" "$tarball_file"; then
        exit 1
    fi
    
    if ! download_file "$checksum_url" "$checksum_file"; then
        exit 1
    fi
    
    # Verify checksum (fails hard on mismatch)
    echo ""
    verify_checksum "$tarball_file" "$checksum_file"
    
    # Extract tarball
    echo ""
    if ! extract_tarball "$tarball_file" "$extract_dir"; then
        exit 1
    fi
    
    # Install main binary
    echo ""
    if ! install_binary "$extract_dir"; then
        exit 1
    fi
    
    # Install helper service (D-Bus, polkit, etc.)
    echo ""
    if ! install_helper "$extract_dir"; then
        exit 1
    fi
    
    # Success!
    echo ""
    print_info "=========================================="
    print_info "CouchPlay $tag_name installed successfully!"
    print_info "=========================================="
    echo ""
    echo "You can now run CouchPlay with:"
    echo "  couchplay"
    echo ""
}

# Run main if script is executed (not sourced)
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
