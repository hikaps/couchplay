#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# CouchPlay Helper Installation Script
#
# This script installs the privileged helper daemon for CouchPlay.
# It must be run with root privileges (sudo).
#
# The helper is required for:
# - Changing input device ownership for player isolation
# - Creating new user accounts for split-screen gaming
# - Configuring PipeWire audio routing
#
# Usage:
#   sudo ./install-helper.sh install    - Install the helper
#   sudo ./install-helper.sh uninstall  - Remove the helper
#   sudo ./install-helper.sh status     - Check installation status
#   ./install-helper.sh export          - Export helper for host install (Flatpak)
#
# Flatpak users: Run 'export' first inside the Flatpak, then run the
# exported script with sudo on the host.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Flatpak detection
IN_FLATPAK=false
if [ -f /.flatpak-info ]; then
    IN_FLATPAK=true
fi

# Installation paths (overridable via environment)
# Use /usr/local for immutable distros (Fedora Silverblue/Kinoite/Bazzite)
# On traditional distros, these can be changed to /usr paths
PREFIX="${PREFIX:-/usr/local}"
LIBEXEC_DIR="${LIBEXEC_DIR:-${PREFIX}/libexec}"
LIB_DIR="${LIB_DIR:-${PREFIX}/lib/couchplay}"
DBUS_SYSTEM_DIR="${DBUS_SYSTEM_DIR:-/etc/dbus-1/system.d}"
DBUS_SERVICE_DIR="${DBUS_SERVICE_DIR:-${PREFIX}/share/dbus-1/system-services}"
SYSTEMD_DIR="${SYSTEMD_DIR:-/etc/systemd/system}"

# Polkit actions usually reside in /usr/share/polkit-1/actions.
# On immutable systems, we try /etc/polkit-1/actions if /usr is read-only.
if [ -z "${POLKIT_DIR:-}" ]; then
    if [ -w "/usr/share/polkit-1/actions" ]; then
        POLKIT_DIR="/usr/share/polkit-1/actions"
    else
        # Fallback for immutable systems (requires Polkit to be configured to read this, or user to layer)
        # Note: If /etc/polkit-1/actions is not scanned by your Polkit version, 
        # you may need to use 'rpm-ostree install' or an overlay.
        POLKIT_DIR="/etc/polkit-1/actions"
        mkdir -p "$POLKIT_DIR"
    fi
fi

# Binary name
HELPER_BINARY="couchplay-helper"

# Export directory: where the helper files are staged for the host-side `sudo ... install`.
# Inside a Flatpak the sandbox home ($HOME) is NOT persisted to the host, so writing to
# $HOME/.local/share silently vanishes (export appears to succeed but no files reach the
# host). Flatpak persists XDG_DATA_HOME to ~/.var/app/<id>/data on the host — the same
# absolute path inside the sandbox — so stage files there. No extra filesystem permission
# is required and the result is host-visible immediately.
if [[ "$IN_FLATPAK" == "true" ]]; then
    EXPORT_DIR="${EXPORT_DIR:-${XDG_DATA_HOME:-${HOME}/.local/share}/couchplay}"
else
    EXPORT_DIR="${EXPORT_DIR:-${HOME}/.local/share/couchplay}"
fi

# Source paths (relative to script location)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Detect structure: release tarball, Flatpak, or build directory
# Flatpak: binary at /app/libexec/, script at /app/share/couchplay/
# Release tarball: script at root, bin/ and data/ are siblings
# Build directory: script in scripts/, build/bin/ and data/ exist
if [[ -f "/app/libexec/${HELPER_BINARY}" ]]; then
    # Flatpak structure
    FLATPAK_LIBEXEC="/app/libexec"
    BINARY_PATH="${FLATPAK_LIBEXEC}/${HELPER_BINARY}"
    DATA_DIR="/app/share/couchplay/data"
elif [[ -f "${SCRIPT_DIR}/bin/${HELPER_BINARY}" ]]; then
    # Release tarball structure
    RELEASE_DIR="${SCRIPT_DIR}"
    BINARY_PATH="${RELEASE_DIR}/bin/${HELPER_BINARY}"
    DATA_DIR="${RELEASE_DIR}/data"
else
    # Build directory structure
    PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
    BINARY_PATH="${PROJECT_DIR}/build/bin/${HELPER_BINARY}"
    DATA_DIR="${PROJECT_DIR}/data"
fi

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        print_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

check_binary() {
    if [[ ! -f "${BINARY_PATH}" ]]; then
        print_error "Helper binary not found at ${BINARY_PATH}"
        print_info "Please build the project first:"
        echo "    mkdir -p build && cd build"
        echo "    cmake .."
        echo "    make"
        exit 1
    fi
}

reload_dbus() {
    # Ask the D-Bus daemon to re-read its configuration so policy files dropped
    # into /etc/dbus-1/system.d/ (e.g. the helper's ownership allow-rule) take
    # effect. Without this the helper cannot register its service name and the
    # Type=dbus systemd unit fails to start. Tries the systemd unit reload first,
    # then falls back to signalling the daemon directly via its pid file.
    if systemctl reload dbus 2>/dev/null; then
        return 0
    fi
    local pidfile dbus_pid
    for pidfile in /run/dbus/pid /var/run/dbus/pid; do
        if [[ -r "$pidfile" ]]; then
            dbus_pid="$(cat "$pidfile" 2>/dev/null || true)"
            if [[ -n "$dbus_pid" ]] && kill -HUP "$dbus_pid" 2>/dev/null; then
                print_warn "systemctl reload dbus unavailable; sent SIGHUP to dbus-daemon (pid ${dbus_pid})."
                return 0
            fi
        fi
    done
    return 1
}

export_helper() {
    # Export helper binary and install script for host-side installation
    # This is used when running from inside a Flatpak
    
    print_info "Exporting CouchPlay helper for host installation..."
    
    # Check for Flatpak binary
    if [[ ! -f "${BINARY_PATH}" ]]; then
        print_error "Helper binary not found at ${BINARY_PATH}"
        exit 1
    fi
    
    # Create export directory
    mkdir -p "${EXPORT_DIR}"
    
    # Copy helper binary
    print_info "Copying helper binary to ${EXPORT_DIR}/"
    install -m755 "${BINARY_PATH}" "${EXPORT_DIR}/${HELPER_BINARY}"
    
    # Copy this script
    print_info "Copying install script to ${EXPORT_DIR}/"
    install -m755 "${BASH_SOURCE[0]}" "${EXPORT_DIR}/install-helper.sh"
    
    # Copy data files (D-Bus config, systemd service, polkit policy)
    if [[ -d "${DATA_DIR}" ]]; then
        print_info "Copying configuration files..."
        mkdir -p "${EXPORT_DIR}/data/dbus"
        mkdir -p "${EXPORT_DIR}/data/polkit"
        
        if [[ -f "${DATA_DIR}/dbus/io.github.hikaps.CouchPlayHelper.conf" ]]; then
            install -m644 "${DATA_DIR}/dbus/io.github.hikaps.CouchPlayHelper.conf" \
                "${EXPORT_DIR}/data/dbus/"
        fi
        if [[ -f "${DATA_DIR}/dbus/io.github.hikaps.CouchPlayHelper.service" ]]; then
            install -m644 "${DATA_DIR}/dbus/io.github.hikaps.CouchPlayHelper.service" \
                "${EXPORT_DIR}/data/dbus/"
        fi
        if [[ -f "${DATA_DIR}/dbus/couchplay-helper.service" ]]; then
            install -m644 "${DATA_DIR}/dbus/couchplay-helper.service" \
                "${EXPORT_DIR}/data/dbus/"
        fi
        if [[ -f "${DATA_DIR}/polkit/io.github.hikaps.couchplay.policy" ]]; then
            install -m644 "${DATA_DIR}/polkit/io.github.hikaps.couchplay.policy" \
                "${EXPORT_DIR}/data/polkit/"
        fi
        if [[ -d "${DATA_DIR}/pipewire" ]]; then
            mkdir -p "${EXPORT_DIR}/data/pipewire"
            install -m644 "${DATA_DIR}/pipewire/50-couchplay.conf" \
                "${EXPORT_DIR}/data/pipewire/"
        fi
    fi
    
    print_info "Export complete!"
    echo ""
    echo "Files exported to: ${EXPORT_DIR}/"
    echo ""
    echo "To install the helper on your host system, run:"
    echo ""
    echo "    sudo ${EXPORT_DIR}/install-helper.sh install"
    echo ""
    echo "Note: The install command requires root privileges to copy files"
    echo "to system directories and enable the systemd service."
}

install_helper() {
    check_root
    check_binary

    print_info "Installing CouchPlay helper daemon..."

    # Install binary
    print_info "Installing binary to ${LIBEXEC_DIR}/"
    install -Dm755 "${BINARY_PATH}" "${LIBEXEC_DIR}/${HELPER_BINARY}"

    # Install bundled runtime libraries, if the release ships them (the binary's
    # RPATH is $ORIGIN/../lib/couchplay, so this lets the helper run without
    # system Qt6/Polkit). Absent for Flatpak/build-dir installs.
    if [[ -d "${SCRIPT_DIR}/lib/couchplay" ]] && compgen -G "${SCRIPT_DIR}/lib/couchplay/*.so*" >/dev/null; then
        print_info "Installing bundled runtime libraries to ${LIB_DIR}/"
        mkdir -p "${LIB_DIR}"
        install -m644 "${SCRIPT_DIR}/lib/couchplay"/*.so* "${LIB_DIR}/"
    fi

    # Install D-Bus configuration
    print_info "Installing D-Bus configuration..."
    install -Dm644 "${DATA_DIR}/dbus/io.github.hikaps.CouchPlayHelper.conf" \
        "${DBUS_SYSTEM_DIR}/io.github.hikaps.CouchPlayHelper.conf"

    # Install D-Bus service file
    install -Dm644 "${DATA_DIR}/dbus/io.github.hikaps.CouchPlayHelper.service" \
        "${DBUS_SERVICE_DIR}/io.github.hikaps.CouchPlayHelper.service"

    # Install systemd service
    print_info "Installing systemd service..."
    install -Dm644 "${DATA_DIR}/dbus/couchplay-helper.service" \
        "${SYSTEMD_DIR}/couchplay-helper.service"

    # Install PolicyKit policy
    print_info "Installing PolicyKit policy..."
    install -Dm644 "${DATA_DIR}/polkit/io.github.hikaps.couchplay.policy" \
        "${POLKIT_DIR}/io.github.hikaps.couchplay.policy"

    # Install PipeWire PulseAudio TCP listener config
    print_info "Installing PipeWire PulseAudio TCP listener..."
    SYSTEM_PIPEWIRE_PULSE_CONF_DIR="${PREFIX}/share/pipewire/pipewire-pulse.conf.d"
    mkdir -p "$SYSTEM_PIPEWIRE_PULSE_CONF_DIR"
    install -Dm644 "${DATA_DIR}/pipewire/50-couchplay.conf" \
        "${SYSTEM_PIPEWIRE_PULSE_CONF_DIR}/50-couchplay.conf"

    # Reload systemd
    print_info "Reloading systemd..."
    systemctl daemon-reload

    # Reload D-Bus so the new ownership policy in /etc/dbus-1/system.d/ takes
    # effect. Without this the helper cannot register its service name and the
    # Type=dbus unit fails to start. Do not silently mask a reload failure.
    print_info "Reloading D-Bus configuration..."
    if ! reload_dbus; then
        print_warn "Could not reload D-Bus configuration automatically."
        print_warn "If the service fails to start, run: sudo systemctl reload dbus  (or reboot)"
    fi

    # Enable and restart service (restart ensures new binary is loaded)
    print_info "Enabling and restarting service..."
    systemctl enable couchplay-helper.service
    if ! systemctl restart couchplay-helper.service; then
        echo ""
        print_error "Failed to start couchplay-helper.service"
        echo ""
        echo "---- journalctl (last 30 lines) ----"
        journalctl -u couchplay-helper.service -n 30 --no-pager 2>&1 || true
        echo "------------------------------------"
        echo ""
        echo "For full logs, run:"
        echo "    journalctl -xeu couchplay-helper.service"
        exit 1
    fi

    print_info "Installation complete!"
    echo ""
    print_info "Helper service status:"
    systemctl status couchplay-helper.service --no-pager || true
}

uninstall_helper() {
    check_root

    print_info "Uninstalling CouchPlay helper daemon..."

    # Stop and disable service
    print_info "Stopping service..."
    systemctl stop couchplay-helper.service 2>/dev/null || true
    systemctl disable couchplay-helper.service 2>/dev/null || true

    # Remove files
    print_info "Removing installed files..."
    rm -f "${LIBEXEC_DIR}/${HELPER_BINARY}"
    rm -f "${DBUS_SYSTEM_DIR}/io.github.hikaps.CouchPlayHelper.conf"
    rm -f "${DBUS_SERVICE_DIR}/io.github.hikaps.CouchPlayHelper.service"
    rm -f "${SYSTEMD_DIR}/couchplay-helper.service"
    rm -f "${POLKIT_DIR}/io.github.hikaps.couchplay.policy"
    rm -f "${PREFIX}/share/pipewire/pipewire-pulse.conf.d/50-couchplay.conf"
    rm -rf "${LIB_DIR}"

    # Reload systemd
    print_info "Reloading systemd..."
    systemctl daemon-reload

    # Reload D-Bus so the removed ownership policy takes effect.
    print_info "Reloading D-Bus configuration..."
    reload_dbus || print_warn "Could not reload D-Bus; changes apply after a reboot."

    print_info "Uninstallation complete!"
}

status_helper() {
    echo "CouchPlay Helper Installation Status"
    echo "====================================="
    echo ""

    # Check binary
    if [[ -f "${LIBEXEC_DIR}/${HELPER_BINARY}" ]]; then
        echo -e "Binary:          ${GREEN}Installed${NC} (${LIBEXEC_DIR}/${HELPER_BINARY})"
    else
        echo -e "Binary:          ${RED}Not installed${NC}"
    fi

    # Check D-Bus config
    if [[ -f "${DBUS_SYSTEM_DIR}/io.github.hikaps.CouchPlayHelper.conf" ]]; then
        echo -e "D-Bus config:    ${GREEN}Installed${NC}"
    else
        echo -e "D-Bus config:    ${RED}Not installed${NC}"
    fi

    # Check D-Bus service
    if [[ -f "${DBUS_SERVICE_DIR}/io.github.hikaps.CouchPlayHelper.service" ]]; then
        echo -e "D-Bus service:   ${GREEN}Installed${NC}"
    else
        echo -e "D-Bus service:   ${RED}Not installed${NC}"
    fi

    # Check systemd service
    if [[ -f "${SYSTEMD_DIR}/couchplay-helper.service" ]]; then
        echo -e "Systemd service: ${GREEN}Installed${NC}"
    else
        echo -e "Systemd service: ${RED}Not installed${NC}"
    fi

    # Check PolicyKit
    if [[ -f "${POLKIT_DIR}/io.github.hikaps.couchplay.policy" ]]; then
        echo -e "PolicyKit:       ${GREEN}Installed${NC}"
    else
        echo -e "PolicyKit:       ${RED}Not installed${NC}"
    fi

    echo ""

    # Check service status
    if systemctl is-active --quiet couchplay-helper.service 2>/dev/null; then
        echo -e "Service status:  ${GREEN}Running${NC}"
    elif systemctl is-enabled --quiet couchplay-helper.service 2>/dev/null; then
        echo -e "Service status:  ${YELLOW}Enabled but not running${NC}"
    else
        echo -e "Service status:  ${RED}Not enabled${NC}"
    fi

    # Test D-Bus connection
    echo ""
    if command -v gdbus &>/dev/null; then
        if gdbus introspect --system --dest io.github.hikaps.CouchPlayHelper \
            --object-path /io/github/hikaps/CouchPlayHelper &>/dev/null; then
            echo -e "D-Bus connection: ${GREEN}Available${NC}"
        else
            echo -e "D-Bus connection: ${RED}Not available${NC}"
        fi
    fi
}

usage() {
    echo "CouchPlay Helper Installation Script"
    echo ""
    echo "Usage: $0 <command>"
    echo ""
    echo "Commands:"
    echo "  export      Export helper files for host installation (run inside Flatpak)"
    echo "  install     Install the helper daemon (requires sudo)"
    echo "  uninstall   Remove the helper daemon (requires sudo)"
    echo "  status      Check installation status"
    echo ""
    if [[ "$IN_FLATPAK" == "true" ]]; then
        echo "Running inside Flatpak detected."
        echo "To install the helper, first run: $0 export"
        echo "Then on the host: sudo ${EXPORT_DIR}/install-helper.sh install"
        echo ""
    fi
    echo "Environment variables:"
    echo "  PREFIX          Installation prefix (default: /usr/local)"
    echo "  LIBEXEC_DIR     Helper binary directory (default: \$PREFIX/libexec)"
    echo "  DBUS_SYSTEM_DIR D-Bus system config dir (default: /etc/dbus-1/system.d)"
    echo "  DBUS_SERVICE_DIR D-Bus service activation dir (default: \$PREFIX/share/dbus-1/system-services)"
    echo "  SYSTEMD_DIR     Systemd unit directory (default: /etc/systemd/system)"
    echo "  POLKIT_DIR      Polkit policy directory (default: /usr/share/polkit-1/actions)"
    echo "  EXPORT_DIR      Where helper files are staged for host install (default: \$XDG_DATA_HOME/couchplay in Flatpak, else ~/.local/share/couchplay)"
}

# Check for Flatpak context and provide guidance
if [[ "$IN_FLATPAK" == "true" ]] && [[ "${1:-}" != "export" ]] && [[ "${1:-}" != "status" ]] && [[ "${1:-}" != "-h" ]] && [[ "${1:-}" != "--help" ]]; then
    print_warn "Running inside Flatpak. The 'install' and 'uninstall' commands require"
    print_warn "host system access. Use the 'export' command first to prepare files for"
    print_warn "host-side installation."
    echo ""
fi

# Main
case "${1:-}" in
    export)
        export_helper
        ;;
    install)
        install_helper
        ;;
    uninstall)
        uninstall_helper
        ;;
    status)
        status_helper
        ;;
    -h|--help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
