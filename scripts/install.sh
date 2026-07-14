#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# CouchPlay One-Liner Installer
#
# Downloads and installs CouchPlay from GitHub releases.
# This script must be run with root privileges.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash -s -- --beta
#   sudo ./install.sh --beta
#
# Requirements:
#   - curl: for downloading files
#   - tar: for extracting the release tarball
#   - sha256sum: for verifying checksums
#   - x86_64 architecture

# Re-run with sudo if not root (allows piped input to work with visible output)
if [[ $EUID -ne 0 ]]; then
    echo "Requesting sudo access to install CouchPlay..."
    # --beta means a develop build, so re-fetch the installer from develop; otherwise main.
    # We re-download because piping leaves no script on disk. Forward "$@" so flags
    # (e.g. --beta) survive the sudo escalation — previously they were silently dropped,
    # which made the documented --beta one-liner always install stable instead of beta.
    if [[ " $* " == *" --beta "* ]]; then
        INSTALLER_BRANCH="develop"
    else
        INSTALLER_BRANCH="main"
    fi
    TMP_SCRIPT=$(mktemp)
    curl -fsSL "https://raw.githubusercontent.com/hikaps/couchplay/${INSTALLER_BRANCH}/scripts/install.sh" > "$TMP_SCRIPT"
    chmod +x "$TMP_SCRIPT"
    exec sudo "$TMP_SCRIPT" "$@"
fi

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
    # Already running as root due to sudo re-exec at script start
    :
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

    # Check for runtime dependencies (warn but don't fail)
    if ! command -v gamescope &>/dev/null; then
        print_warn "gamescope is not installed. CouchPlay requires gamescope to launch game sessions."
        echo ""
        echo "On Debian/Ubuntu:"
        echo "  sudo apt install gamescope"
        echo ""
        echo "On Fedora:"
        echo "  sudo dnf install gamescope"
        echo ""
        echo "On Arch Linux:"
        echo "  sudo pacman -S gamescope"
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

check_binary_deps() {
    # The release tarball does NOT bundle runtime libraries; the helper and GUI link
    # against system Qt6, KDE Frameworks 6, Polkit and PipeWire. If a required shared
    # library is missing the helper fails to start with an opaque exit code (e.g. the
    # "Main process exited" / ERRNO 2 seen on minimal Arch installs). Detect missing
    # libs up front so the user gets a clear, actionable message instead.
    local extract_dir="$1"
    local bin_dir helper missing
    bin_dir=$(find "$extract_dir" -type d -name "bin" | head -1)
    [[ -z "$bin_dir" ]] && return 0
    helper="${bin_dir}/couchplay-helper"
    [[ -x "$helper" ]] || return 0

    if ! missing=$(ldd "$helper" 2>/dev/null | grep -i 'not found'); then
        return 0
    fi
    [[ -z "$missing" ]] && return 0

    print_error "The helper binary is missing required shared libraries on this system:"
    echo ""
    echo "$missing" | sed -E 's/^[[:space:]]+//; s/[[:space:]]*=>.*//' | sort -u \
        | while IFS= read -r lib; do [[ -n "$lib" ]] && echo "  $lib"; done
    echo ""
    echo "The CouchPlay release does not bundle runtime libraries; it links against"
    echo "system Qt6, KDE Frameworks 6, Polkit and PipeWire. Install the packages"
    echo "providing the libraries listed above, then re-run this installer."
    echo ""
    echo "On Arch Linux / CachyOS:"
    echo "  sudo pacman -S qt6-base polkit-qt6 kirigami pipewire"
    echo ""
    echo "On Fedora:"
    echo "  sudo dnf install qt6-qtbase polkit-qt6-1-devel kf6-kirigami pipewire"
    echo ""
    echo "On Debian/Ubuntu:"
    echo "  sudo apt install qt6-base-dev libpolkit-qt6-1-1 kirigami pipewire"
    exit 1
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

get_beta_release() {
    # Fetches the beta (pre-release) metadata from GitHub Releases API
    # Uses the /releases/tags/beta endpoint to get the rolling beta release
    # Returns JSON with: tag_name, name, assets[], etc.

    local api_url="${GITHUB_API}/repos/${REPO_OWNER}/${REPO_NAME}/releases/tags/beta"
    local response
    local http_code

    print_info "Fetching beta release information..."

    response=$(curl -sL -w "\n%{http_code}" "$api_url" 2>/dev/null)
    http_code=$(echo "$response" | tail -n1)
    response=$(echo "$response" | sed '$d')

    if [[ "$http_code" != "200" ]]; then
        print_error "Failed to fetch beta release information (HTTP $http_code)"
        echo ""
        echo "This could mean:"
        echo "  - No beta release has been published yet"
        echo "  - GitHub API rate limit exceeded"
        echo "  - Network connectivity issues"
        echo ""
        echo "Please check: https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/tag/beta"
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

install_data() {
    # Installs desktop file, icon, and metainfo for desktop integration
    local extract_dir="$1"
    
    # Find the extracted directory containing data/
    local data_dir
    data_dir=$(find "$extract_dir" -type d -name "data" | head -1)
    
    if [[ -z "$data_dir" ]]; then
        print_warn "Could not find data/ directory in extracted tarball — skipping desktop integration"
        return 0
    fi
    
    # Install desktop file
    local desktop_src="${data_dir}/io.github.hikaps.couchplay.desktop"
    if [[ -f "$desktop_src" ]]; then
        print_info "Installing desktop file..."
        install -Dm644 "$desktop_src" "${PREFIX}/share/applications/io.github.hikaps.couchplay.desktop"
    fi
    
    # Install icon
    local icon_src="${data_dir}/icons/io.github.hikaps.couchplay.png"
    if [[ -f "$icon_src" ]]; then
        print_info "Installing icon..."
        install -Dm644 "$icon_src" "${PREFIX}/share/icons/hicolor/512x512/apps/io.github.hikaps.couchplay.png"
    fi
    
    # Install metainfo
    local metainfo_src="${data_dir}/io.github.hikaps.couchplay.metainfo.xml"
    if [[ -f "$metainfo_src" ]]; then
        print_info "Installing metainfo..."
        install -Dm644 "$metainfo_src" "${PREFIX}/share/metainfo/io.github.hikaps.couchplay.metainfo.xml"
    fi
    
    # Update icon cache and desktop database (non-fatal)
    if command -v gtk-update-icon-cache &>/dev/null; then
        gtk-update-icon-cache -f -t "${PREFIX}/share/icons/hicolor" 2>/dev/null || true
    fi
    if command -v update-desktop-database &>/dev/null; then
        update-desktop-database "${PREFIX}/share/applications" 2>/dev/null || true
    fi
    
    print_info "Desktop integration installed successfully"
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

# Determine the real user calling the script (for SteamOS home directory persistence)
REAL_USER="${SUDO_USER:-deck}"
REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
REAL_HOME="${REAL_HOME:-/home/deck}"

# =============================================================================
# Installation Pathways
# =============================================================================

install_sysext() {
    local BETA="$1"
    local release_json="$2"
    local tag_name="$3"
    
    print_info "Installing via SteamOS System Extension (sysext)..."
    
    # Get asset URLs
    local raw_url checksum_url flatpak_url
    raw_url=$(get_asset_url "$release_json" "couchplay\.steamos\.raw")
    checksum_url=$(get_asset_url "$release_json" "couchplay\.steamos\.sha256")
    flatpak_url=$(get_asset_url "$release_json" "couchplay\.flatpak")
    
    if [[ -z "$raw_url" ]]; then
        print_error "Could not find couchplay.steamos.raw asset in release"
        exit 1
    fi
    if [[ -z "$checksum_url" ]]; then
        print_error "Could not find couchplay.steamos.sha256 asset in release"
        exit 1
    fi
    if [[ -z "$flatpak_url" ]]; then
        print_error "Could not find couchplay.flatpak asset in release"
        exit 1
    fi
    
    # Setup temporary directory and cleanup trap
    TEMP_DIR=$(mktemp -d)
    trap cleanup EXIT
    
    local raw_file="${TEMP_DIR}/couchplay.steamos.raw"
    local checksum_file="${TEMP_DIR}/couchplay.steamos.sha256"
    local flatpak_file="${TEMP_DIR}/couchplay.flatpak"
    
    # Download files
    if ! download_file "$raw_url" "$raw_file"; then
        exit 1
    fi
    if ! download_file "$checksum_url" "$checksum_file"; then
        exit 1
    fi
    if ! download_file "$flatpak_url" "$flatpak_file"; then
        exit 1
    fi
    
    # Verify checksum
    verify_checksum "$raw_file" "$checksum_file"
    
    # 1. Install Flatpak
    print_info "Installing Flatpak bundle..."
    if ! command -v flatpak &>/dev/null; then
        print_error "flatpak command not found. Please install flatpak first."
        exit 1
    fi
    print_info "Ensuring org.kde.Platform 6.10 is installed..."
    sudo -u "$REAL_USER" flatpak install --user --noninteractive -y flathub org.kde.Platform/x86_64/6.10
    sudo -u "$REAL_USER" flatpak install --user --noninteractive -y "$flatpak_file"
    
    # Stop existing CouchPlay helper service and systemd-sysext before upgrading
    print_info "Stopping active CouchPlay services..."
    systemctl stop couchplay-helper.service || true
    systemctl stop systemd-sysext || true
    
    # Clear out any legacy layout folders or old raw files
    rm -rf "$REAL_HOME/.couchplay-extension"
    rm -f "$REAL_HOME/.couchplay.raw"
    rm -f "$REAL_HOME/.couchplay.steamos.raw"
    rm -f /var/lib/extensions/couchplay.raw
    rm -f /var/lib/extensions/couchplay.steamos.raw
    
    # Deploy the new pre-built extension
    print_info "Deploying system extension..."
    mv "$raw_file" "$REAL_HOME/.couchplay.steamos.raw"
    chown "$REAL_USER:$REAL_USER" "$REAL_HOME/.couchplay.steamos.raw"
    
    mkdir -p /var/lib/extensions
    ln -s "$REAL_HOME/.couchplay.steamos.raw" /var/lib/extensions/couchplay.steamos.raw
    
    # Load and enable the system extension
    print_info "Merging system extension..."
    systemctl enable systemd-sysext
    systemctl restart systemd-sysext
    
    # Reload D-Bus configuration to discover the new system service policy
    print_info "Reloading D-Bus daemon..."
    systemctl reload dbus
    
    # Start the helper daemon
    print_info "Starting couchplay-helper service..."
    systemctl daemon-reload
    systemctl restart couchplay-helper.service
    
    # Configure controller hidraw udev rules
    print_info "Configuring udev rules..."
    echo 'KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="054c", ATTRS{idProduct}=="0ce6", MODE="0666", TAG+="uaccess", TAG+="seat"' | tee /etc/udev/rules.d/99-couchplay-hidraw.rules
    udevadm control --reload-rules
    udevadm trigger
    
    echo ""
    print_info "=========================================="
    print_info "CouchPlay $tag_name installed successfully!"
    print_info "=========================================="
    echo ""
    echo "You can now run CouchPlay from your application launcher or terminal:"
    echo "  couchplay"
    echo ""
}

install_tarball() {
    local BETA="$1"
    local release_json="$2"
    local tag_name="$3"
    
    print_info "Installing via traditional release tarball..."
    
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

    # Verify the helper's runtime libraries are present (the tarball doesn't bundle them).
    check_binary_deps "$extract_dir"
    
    # Install main binary
    echo ""
    if ! install_binary "$extract_dir"; then
        exit 1
    fi

    # Install desktop file, icon, and metainfo
    echo ""
    if ! install_data "$extract_dir"; then
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

# =============================================================================
# Main
# =============================================================================

main() {
    local BETA=false
    local FORCE_SYSEXT=false
    local FORCE_TARBALL=false
    
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --beta) BETA=true; shift ;;
            --sysext) FORCE_SYSEXT=true; shift ;;
            --tarball) FORCE_TARBALL=true; shift ;;
            *) shift ;;
        esac
    done

    print_info "CouchPlay Installer"
    echo ""
    
    # Pre-flight checks
    check_root
    check_dependencies
    check_architecture
    
    echo ""
    
    # Get release info (beta or stable)
    local release_json
    if $BETA; then
        release_json=$(get_beta_release)
        print_warn "Installing BETA build from develop — not a stable release!"
    else
        release_json=$(get_latest_release)
    fi
    
    local tag_name
    tag_name=$(get_release_tag "$release_json")
    print_info "Latest release: $tag_name"
    
    # Determine installation pathway
    local USE_SYSEXT=false
    if $FORCE_SYSEXT; then
        USE_SYSEXT=true
    elif $FORCE_TARBALL; then
        USE_SYSEXT=false
    else
        # Auto-detect SteamOS
        if [[ -f /etc/os-release ]] && grep -q "ID=steamos" /etc/os-release; then
            USE_SYSEXT=true
        fi
    fi
    
    if $USE_SYSEXT; then
        install_sysext "$BETA" "$release_json" "$tag_name"
    else
        install_tarball "$BETA" "$release_json" "$tag_name"
    fi
}

# Run main if script is executed (not sourced)
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
