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

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Installation paths
# Use /usr/local for immutable distros (Fedora Silverblue/Kinoite/Bazzite)
# On traditional distros, these can be changed to /usr paths
LIBEXEC_DIR="/usr/local/libexec"
DBUS_SYSTEM_DIR="/etc/dbus-1/system.d"
DBUS_SERVICE_DIR="/usr/local/share/dbus-1/system-services"
SYSTEMD_DIR="/etc/systemd/system"
POLKIT_DIR="/usr/local/share/polkit-1/actions"

# Source paths (relative to script location)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
DATA_DIR="${PROJECT_DIR}/data"

# Binary name
HELPER_BINARY="couchplay-helper"

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
    if [[ ! -f "${BUILD_DIR}/bin/${HELPER_BINARY}" ]]; then
        print_error "Helper binary not found at ${BUILD_DIR}/bin/${HELPER_BINARY}"
        print_info "Please build the project first:"
        echo "    mkdir -p build && cd build"
        echo "    cmake .."
        echo "    make"
        exit 1
    fi
}

install_helper() {
    check_root
    check_binary

    print_info "Installing CouchPlay helper daemon..."

    # Install binary
    print_info "Installing binary to ${LIBEXEC_DIR}/"
    install -Dm755 "${BUILD_DIR}/bin/${HELPER_BINARY}" "${LIBEXEC_DIR}/${HELPER_BINARY}"

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

    # Reload systemd
    print_info "Reloading systemd..."
    systemctl daemon-reload

    # Reload D-Bus
    print_info "Reloading D-Bus configuration..."
    systemctl reload dbus 2>/dev/null || true

    # Enable and restart service (restart ensures new binary is loaded)
    print_info "Enabling and restarting service..."
    systemctl enable couchplay-helper.service
    systemctl restart couchplay-helper.service

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

    # Reload systemd
    print_info "Reloading systemd..."
    systemctl daemon-reload

    # Reload D-Bus
    print_info "Reloading D-Bus configuration..."
    systemctl reload dbus 2>/dev/null || true

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
    echo "Usage: sudo $0 <command>"
    echo ""
    echo "Commands:"
    echo "  install     Install the helper daemon"
    echo "  uninstall   Remove the helper daemon"
    echo "  status      Check installation status"
    echo ""
}

# Main
case "${1:-}" in
    install)
        install_helper
        ;;
    uninstall)
        uninstall_helper
        ;;
    status)
        status_helper
        ;;
    *)
        usage
        exit 1
        ;;
esac
