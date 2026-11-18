#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# CouchPlay One-Liner Installer
#
# Downloads and installs CouchPlay from GitHub releases.
# Runs as your own user. Only --tarball (a system-wide install) and, on systems that block user
# namespaces for bubblewrap, the one AppArmor step ask for sudo.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/main/scripts/install.sh | bash -s -- --beta
#   ./install.sh --beta
#
# Requirements:
#   - curl: for downloading files
#   - tar: for extracting the release tarball
#   - sha256sum: for verifying checksums
#   - x86_64 architecture
#
# Host requirements for running players (checked after install, warns only):
#   bubblewrap (bwrap), dbus (dbus-run-session), gamescope,
#   and a user-writable /dev/uinput.

INSTALLER_ARGS=("$@")

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


# Only the system-wide tarball install needs root: re-run this installer once under sudo.
# Piping leaves no script on disk, so it is downloaded again. Flags are forwarded; --beta means a
# develop build, so the installer is fetched from develop, otherwise from main.
reexec_with_sudo() {
    echo "The tarball install writes under ${PREFIX}; requesting sudo access..."
    local branch="main" tmp_script
    if [[ " ${INSTALLER_ARGS[*]} " == *" --beta "* ]]; then
        branch="develop"
    fi
    tmp_script=$(mktemp)
    curl -fsSL "https://raw.githubusercontent.com/hikaps/couchplay/${branch}/scripts/install.sh" > "$tmp_script"
    chmod +x "$tmp_script"
    exec sudo "$tmp_script" "${INSTALLER_ARGS[@]}"
}

# Run a command as the desktop user (a plain call unless the script itself runs as root).
as_user() {
    if [[ $EUID -eq 0 ]]; then
        sudo -u "$REAL_USER" "$@"
    else
        "$@"
    fi
}

# Run a command as root, asking for sudo only when needed.
as_root() {
    if [[ $EUID -eq 0 ]]; then
        "$@"
    else
        sudo "$@"
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

check_binary_deps() {
    # The release tarball does NOT bundle runtime libraries; the binaries link against
    # system Qt6, KDE Frameworks 6 and PipeWire. If a required shared library is missing
    # the program fails to start with an opaque error. Detect missing libs up front so
    # the user gets a clear, actionable message instead.
    local extract_dir="$1"
    local bin_dir bin missing
    bin_dir=$(find "$extract_dir" -type d -name "bin" | head -1)
    [[ -z "$bin_dir" ]] && return 0

    for bin in couchplay couchplay-vdisplay couchplay-padfwd; do
        [[ -x "${bin_dir}/${bin}" ]] || continue
        if ! missing=$(ldd "${bin_dir}/${bin}" 2>/dev/null | grep -i 'not found'); then
            continue
        fi
        [[ -z "$missing" ]] && continue

        print_error "${bin} is missing required shared libraries on this system:"
        echo ""
        echo "$missing" | sed -E 's/^[[:space:]]+//; s/[[:space:]]*=>.*//' | sort -u \
            | while IFS= read -r lib; do [[ -n "$lib" ]] && echo "  $lib"; done
        echo ""
        echo "The CouchPlay release does not bundle runtime libraries; it links against"
        echo "system Qt6, KDE Frameworks 6 and PipeWire. Install the packages"
        echo "providing the libraries listed above, then re-run this installer."
        echo ""
        echo "On Arch Linux / CachyOS:"
        echo "  sudo pacman -S qt6-base kirigami pipewire"
        echo ""
        echo "On Fedora:"
        echo "  sudo dnf install qt6-qtbase kf6-kirigami pipewire"
        echo ""
        echo "On Debian/Ubuntu:"
        echo "  sudo apt install qt6-base-dev kirigami pipewire"
        exit 1
    done
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
    # Installs the main couchplay binary and gamemode launcher to BIN_DIR
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
    
    # Install the Game Mode launcher script if present (ships at the tarball root).
    local tarball_root
    tarball_root="$(dirname "$bin_dir")"
    if [[ -f "${tarball_root}/couchplay-gamemode.sh" ]]; then
        print_info "Installing Game Mode launcher to ${BIN_DIR}"
        install -Dm755 "${tarball_root}/couchplay-gamemode.sh" "${BIN_DIR}/couchplay-gamemode"
    fi
    
    # Install the host tools (run on the host by CouchPlay; found via ../libexec or LIBEXEC_DIR)
    local tool
    for tool in couchplay-vdisplay couchplay-padfwd; do
        if [[ -f "${bin_dir}/${tool}" ]]; then
            print_info "Installing ${tool} to ${LIBEXEC_DIR}"
            if ! install -Dm755 "${bin_dir}/${tool}" "${LIBEXEC_DIR}/${tool}"; then
                print_error "Failed to install ${tool}"
                return 1
            fi
        else
            print_error "${tool} missing from the tarball"
            return 1
        fi
    done

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

setup_userns_access() {
    # Ubuntu 23.10+ sets kernel.apparmor_restrict_unprivileged_userns=1, which makes bwrap fail with
    # "setting up uid map: Permission denied". If that happens, allow user namespaces for bwrap only
    # (a narrow AppArmor profile, not the global sysctl) and verify that it worked.
    local bwrap_path
    bwrap_path="$(command -v bwrap 2>/dev/null || true)"
    [[ -n "$bwrap_path" ]] || return 0
    bwrap_path="$(readlink -f "$bwrap_path")"
    id -u "$REAL_USER" &>/dev/null || {
        print_warn "Cannot determine the desktop user; skipping the user-namespace check."
        return 0
    }

    local probe=(bwrap --unshare-user --uid "$(id -u "$REAL_USER")" --gid "$(id -g "$REAL_USER")" --dev-bind / / true)
    if as_user "${probe[@]}" &>/dev/null; then
        return 0
    fi
    [[ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo 0)" == "1" ]] || {
        print_warn "bwrap cannot create a user namespace here. CouchPlay needs unprivileged user namespaces; see the README troubleshooting section."
        return 0
    }
    local apparmor_parser
    apparmor_parser="$(command -v apparmor_parser 2>/dev/null || true)"
    [[ -z "$apparmor_parser" && -x /usr/sbin/apparmor_parser ]] && apparmor_parser=/usr/sbin/apparmor_parser
    if [[ -z "$apparmor_parser" ]]; then
        print_warn "Your system blocks user namespaces for bwrap (AppArmor) and 'apparmor_parser' is missing. See the README troubleshooting section."
        return 0
    fi

    local profile=/etc/apparmor.d/bwrap-couchplay
    print_info "Your system blocks user namespaces for bwrap (AppArmor). Allowing it for $bwrap_path only: $profile (needs sudo)"
    if ! as_root tee "$profile" >/dev/null <<EOF
# Installed by the CouchPlay installer: lets bubblewrap create user namespaces under
# kernel.apparmor_restrict_unprivileged_userns=1. Remove this file and run
# 'sudo apparmor_parser -R /etc/apparmor.d/bwrap-couchplay' to undo it.
abi <abi/4.0>,
include <tunables/global>

profile bwrap-couchplay $bwrap_path flags=(unconfined) {
  userns,
  include if exists <local/bwrap-couchplay>
}
EOF
    then
        print_warn "Could not write $profile. See the README troubleshooting section."
        return 0
    fi
    if as_root "$apparmor_parser" -r "$profile" && as_user "${probe[@]}" &>/dev/null; then
        print_info "bwrap can create user namespaces now."
    else
        as_root "$apparmor_parser" -R "$profile" &>/dev/null || true
        as_root rm -f "$profile"
        print_warn "Could not enable user namespaces for bwrap automatically. See the README troubleshooting section."
    fi
}

check_host_requirements() {
    # Players run in a bubblewrap sandbox on the host. Warn (never fail) about
    # missing tools so the user knows what to install.
    local missing=false
    local -a checks=(
        "bwrap:bubblewrap"
        "dbus-run-session:dbus"
        "gamescope:gamescope"
    )
    local entry cmd pkg
    for entry in "${checks[@]}"; do
        cmd="${entry%%:*}"
        pkg="${entry##*:}"
        if ! command -v "$cmd" &>/dev/null; then
            print_warn "'$cmd' not found on the host (package: $pkg). CouchPlay needs it to run player sandboxes."
            missing=true
        fi
    done

    if ! as_user test -w /dev/uinput 2>/dev/null; then
        print_warn "/dev/uinput is not writable by $REAL_USER. CouchPlay needs it to create one virtual controller per player."
        echo "         Install your distro's Steam udev rules (e.g. the 'steam-devices' package) or add a udev rule"
        echo "         that grants your user access to /dev/uinput (KERNEL==\"uinput\", TAG+=\"uaccess\")."
    fi

    setup_userns_access

    if $missing; then
        echo ""
        echo "Install the missing packages, e.g.:"
        echo "  Fedora/Bazzite:  sudo dnf install bubblewrap dbus-daemon gamescope"
        echo "  Arch/CachyOS:    sudo pacman -S bubblewrap dbus gamescope"
        echo "  Debian/Ubuntu:   sudo apt install bubblewrap dbus gamescope"
        echo "  SteamOS:         check 'which bwrap dbus-run-session gamescope'; see the README"
    fi
}

# True when the old 'couchplay' group still has members or accounts using it as their primary group.
# The migration script leaves an empty group behind, which must not count as an installed helper.
legacy_group_in_use() {
    local entry gid
    entry="$(getent group couchplay)" || return 1
    [[ -n "${entry##*:}" ]] && return 0
    gid="$(cut -d: -f3 <<<"$entry")"
    getent passwd | awk -F: -v gid="$gid" '$4 == gid { f = 1 } END { exit !f }'
}

check_legacy_helper() {
    # Versions before the rootless release ran players as Linux accounts through a root helper service.
    # If that is still installed, point the user at the migration script (it never runs automatically).
    local beta="$1" tarball="$2"
    local found=false
    if [[ -e /etc/systemd/system/couchplay-helper.service ]] \
        || [[ -e /usr/local/libexec/couchplay-helper ]] \
        || [[ -e /usr/libexec/couchplay-helper ]] \
        || legacy_group_in_use; then
        found=true
    fi
    $found || return 0

    local branch="main" flatpak_flag=" --flatpak"
    [[ "$beta" == "true" ]] && branch="develop"
    [[ "$tarball" == "true" ]] && flatpak_flag=""
    echo ""
    print_warn "An older CouchPlay helper (root service and player accounts) is still installed."
    echo "         This version no longer uses it: players now run in a sandbox as your own user."
    echo "         Your existing players, their logins and saves are NOT imported automatically."
    echo "         Run the migration script to copy them over and remove the old helper:"
    echo ""
    echo "           curl -fsSL https://raw.githubusercontent.com/hikaps/couchplay/${branch}/scripts/migrate-from-helper.sh -o migrate-from-helper.sh"
    echo "           sudo bash migrate-from-helper.sh --user ${REAL_USER}${flatpak_flag}          # dry run, lists players"
    echo "           sudo bash migrate-from-helper.sh --user ${REAL_USER}${flatpak_flag} --yes    # do it"
    echo ""
    echo "         Details: README, section \"Upgrading from the helper\"."
}

cleanup() {
    # Cleans up the temporary directory
    # Safe to call multiple times
    if [[ -n "${TEMP_DIR:-}" && -d "$TEMP_DIR" ]]; then
        print_info "Cleaning up temporary files..."
        rm -rf "$TEMP_DIR"
    fi
}

# The desktop user the sandboxes and Flatpak install belong to. As root (sudo, or the --tarball re-exec)
# that is the user who ran sudo; otherwise it is whoever runs the script.
if [[ $EUID -eq 0 ]]; then
    REAL_USER="${SUDO_USER:-deck}"
else
    REAL_USER="$(id -un)"
fi
REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
REAL_HOME="${REAL_HOME:-/home/deck}"

# =============================================================================
# Installation Pathways
# =============================================================================

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

    # Verify the runtime libraries are present (the tarball doesn't bundle them).
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

    # Host runtime requirements are checked after installation (see check_host_requirements).
    
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

install_flatpak() {
    local BETA="$1"
    local release_json="$2"
    local tag_name="$3"

    print_info "Installing via Flatpak (recommended - runs on every distro)..."

    if ! command -v flatpak &>/dev/null; then
        print_error "flatpak not found. Install it, then re-run:"
        echo "  Fedora/RHEL:    sudo dnf install flatpak"
        echo "  Arch/CachyOS:   sudo pacman -S flatpak"
        echo "  Debian/Ubuntu:  sudo apt install flatpak"
        echo "  Or install the native build instead: re-run with --tarball"
        exit 1
    fi

    local flatpak_url flatpak_sha_url
    flatpak_url=$(get_asset_url "$release_json" "couchplay\.flatpak")
    flatpak_sha_url=$(get_asset_url "$release_json" "couchplay\.flatpak\.sha256")
    if [[ -z "$flatpak_url" ]]; then
        print_error "Could not find couchplay.flatpak asset in release $tag_name"
        exit 1
    fi

    TEMP_DIR=$(mktemp -d)
    chmod 755 "$TEMP_DIR"  # real-user flatpak install needs traversal
    trap cleanup EXIT

    local flatpak_file="${TEMP_DIR}/couchplay.flatpak"

    print_info "Flatpak bundle: $flatpak_url"
    if ! download_file "$flatpak_url" "$flatpak_file"; then
        exit 1
    fi

    # Verify checksum when the release ships one.
    if [[ -n "$flatpak_sha_url" ]]; then
        local sha_file="${TEMP_DIR}/couchplay.flatpak.sha256"
        if ! download_file "$flatpak_sha_url" "$sha_file"; then
            print_error "Could not download checksum. Refusing to install without checksum verification"
            exit 1
        fi
        verify_checksum "$flatpak_file" "$sha_file"
    fi

    # Flatpak always installs for the desktop user (--user); no root is involved.
    print_info "Ensuring Flathub remote + org.kde.Platform 6.10 runtime..."
    as_user flatpak remote-add --user --if-not-exists flathub \
        https://flathub.org/repo/flathub.flatpakrepo || true
    as_user flatpak install --user --noninteractive -y \
        flathub org.kde.Platform/x86_64/6.10

    print_info "Installing CouchPlay Flatpak bundle..."
    as_user flatpak install --user --noninteractive -y "$flatpak_file"

    # The Flatpak ships its own couchplay-padfwd / couchplay-vdisplay and runs them from
    # its app path on the host, so nothing else needs to be installed.

    echo ""
    print_info "=========================================="
    print_info "CouchPlay $tag_name installed successfully!"
    print_info "=========================================="
    echo ""
    echo "Launch CouchPlay from your application menu, or run:"
    echo "  flatpak run io.github.hikaps.couchplay"
    echo ""
    echo "Prefer the native binary (Fedora-family only)? Re-run with --tarball."
    echo ""
}

# =============================================================================
# Main
# =============================================================================

main() {
    local BETA=false
    local FORCE_TARBALL=false
    local FORCE_FLATPAK=false
    
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --beta) BETA=true; shift ;;
            --flatpak) FORCE_FLATPAK=true; shift ;;
            --tarball) FORCE_TARBALL=true; shift ;;
            --sysext) print_warn "--sysext was removed; installing via Flatpak instead."; shift ;;
            *) shift ;;
        esac
    done

    # --tarball writes under ${PREFIX}, so it is the one path that needs root.
    if $FORCE_TARBALL && [[ $EUID -ne 0 ]]; then
        reexec_with_sudo
    fi

    print_info "CouchPlay Installer"
    echo ""
    
    # Pre-flight checks
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
    
    # --tarball forces the native install; every other distro (including SteamOS) uses the Flatpak.
    if $FORCE_TARBALL; then
        install_tarball "$BETA" "$release_json" "$tag_name"
    else
        install_flatpak "$BETA" "$release_json" "$tag_name"
    fi

    check_host_requirements
    check_legacy_helper "$BETA" "$FORCE_TARBALL"
}

# Run main when executed, including piped from curl (BASH_SOURCE is empty then); not when sourced.
if [[ -z "${BASH_SOURCE[0]:-}" || "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
