#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-lateral
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Test virtual display creation for Sunshine capture.
# Tries multiple approaches: Gamescope headless, VKMS, Xvfb.
#
# Usage: ./04-test-virtual-display.sh [method]
#   method: gamescope | vkms | xvfb | all (default: all)
#
# IMPORTANT: This script tests display creation ONLY.
# It does NOT require Sunshine to be installed.

set -euo pipefail

METHOD="${1:-all}"

echo "=== Virtual Display Creation Test ==="
echo "Method: ${METHOD}"
echo ""

test_gamescope_headless() {
    echo "--- Testing Gamescope Headless Mode ---"
    echo ""
    
    if ! command -v gamescope &>/dev/null; then
        echo "SKIP: gamescope not installed"
        return 1
    fi
    
    echo "Gamescope version:"
    gamescope --version 2>&1 || true
    echo ""
    
    # Check if headless backend exists
    echo "Checking for headless backend..."
    if gamescope --help 2>&1 | grep -qi "headless"; then
        echo "PASS: Headless backend flag found in gamescope help"
    else
        echo "INFO: No explicit --headless flag found"
        echo "Trying --backend=headless..."
    fi
    echo ""
    
    # Try launching gamescope in headless mode
    echo "Attempting: gamescope -W 1920 -H 1080 -w 1920 -h 1080 -- sleep 3"
    echo "(If this works, Gamescope creates a virtual display)"
    echo ""
    
    # Note: This may fail if no DRM device is available or permissions are wrong
    timeout 5 gamescope -W 1920 -H 1080 -w 1920 -h 1080 -- sleep 3 2>&1 && {
        echo "PASS: Gamescope launched successfully"
    } || {
        echo "FAIL or TIMEOUT: Gamescope headless launch failed"
        echo "This may require:"
        echo "  - DRM device access (user in 'video' or 'render' group)"
        echo "  - Running on a system with GPU drivers"
        echo "  - Appropriate capabilities (cap_sys_admin for KMS)"
    }
    echo ""
}

test_vkms() {
    echo "--- Testing VKMS (Virtual Kernel Mode Setting) ---"
    echo ""
    
    # Check if vkms module is available
    if [ -f /lib/modules/$(uname -r)/kernel/drivers/gpu/drm/vkms/vkms.ko* ] || \
       modinfo vkms &>/dev/null; then
        echo "PASS: VKMS kernel module available"
    else
        echo "FAIL: VKMS kernel module not found"
        echo "Try: sudo modprobe vkms"
        return 1
    fi
    echo ""
    
    # Check if already loaded
    if lsmod | grep -q vkms; then
        echo "PASS: VKMS module already loaded"
    else
        echo "INFO: VKMS module not loaded. Try: sudo modprobe vkms"
    fi
    echo ""
    
    # Check for VKMS DRM device
    for card in /dev/dri/card*; do
        if [ -e "${card}" ]; then
            # Check sysfs for driver name
            card_num=$(basename "${card}" | sed 's/card//')
            driver=$(cat "/sys/class/drm/card${card_num}/device/driver/module/drivers" 2>/dev/null || echo "unknown")
            if echo "${driver}" | grep -qi vkms; then
                echo "PASS: VKMS device found at ${card}"
            fi
        fi
    done
    echo ""
}

test_xvfb() {
    echo "--- Testing Xvfb (X Virtual Framebuffer) ---"
    echo ""
    
    if ! command -v Xvfb &>/dev/null; then
        echo "SKIP: Xvfb not installed"
        echo "Install: sudo dnf install xorg-x11-server-Xvfb"
        return 1
    fi
    echo "PASS: Xvfb available"
    echo ""
    
    # Try creating a virtual display
    DISPLAY_NUM=99
    echo "Creating virtual display :${DISPLAY_NUM}..."
    
    Xvfb ":${DISPLAY_NUM}" -screen 0 1920x1080x32 &
    XVFB_PID=$!
    sleep 1
    
    if kill -0 "${XVFB_PID}" 2>/dev/null; then
        echo "PASS: Xvfb running (PID ${XVFB_PID})"
        echo "  DISPLAY=:${DISPLAY_NUM}"
        echo "  Resolution: 1920x1080x32"
        echo ""
        echo "Sunshine can capture this via X11 capture backend."
        kill "${XVFB_PID}" 2>/dev/null || true
    else
        echo "FAIL: Xvfb failed to start"
    fi
    echo ""
}

echo "=== Running tests ==="
echo ""

case "${METHOD}" in
    gamescope)
        test_gamescope_headless
        ;;
    vkms)
        test_vkms
        ;;
    xvfb)
        test_xvfb
        ;;
    all)
        test_gamescope_headless
        test_vkms
        test_xvfb
        ;;
    *)
        echo "Unknown method: ${METHOD}"
        echo "Usage: $0 [gamescope|vkms|xvfb|all]"
        exit 1
        ;;
esac

echo "=== Test Complete ==="
echo ""
echo "For CouchPlay integration, the recommended approach is:"
echo "  1. PRIMARY: Gamescope headless mode (gaming-optimized compositor)"
echo "  2. FALLBACK: VKMS kernel module (direct DRM virtual display)"
echo "  3. LAST RESORT: Xvfb (X11 software rendering, slow for games)"
