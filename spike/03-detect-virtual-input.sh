#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Detect Sunshine virtual input devices.
# Run this while Sunshine is streaming to a Moonlight client.
# Documents the virtual device names/patterns that Sunshine creates.
#
# Usage: ./03-detect-virtual-input.sh
#
# Expected output: Lists all virtual input devices with their names,
# event numbers, and identification details.

set -euo pipefail

echo "=== Sunshine Virtual Input Device Detection ==="
echo ""
echo "This script detects virtual input devices created by Sunshine."
echo "Run while Sunshine is actively streaming to a Moonlight client."
echo ""

echo "=== Known Sunshine Virtual Device Names ==="
echo ""
echo "Gamepads (created via inputtino/libevdev on Linux):"
echo "  - 'Sunshine X-Box One (virtual) pad'   (vendor: 0x045E, product: 0x02EA)"
echo "  - 'Sunshine Nintendo (virtual) pad'     (vendor: 0x057E, product: 0x2009)"
echo "  - 'Sunshine PS5 (virtual) pad'          (vendor: 0x054C, product: 0x0CE6)"
echo ""
echo "Other devices (all vendor: 0xBEEF, product: 0xDEAD):"
echo "  - 'Mouse passthrough'"
echo "  - 'Keyboard passthrough'"
echo "  - 'Touch passthrough'"
echo "  - 'Pen passthrough'"
echo ""
echo "Detection pattern: device name starts with 'Sunshine ' OR"
echo "                   name contains 'passthrough' with vendor 0xBEEF"
echo ""

echo "=== Scanning /proc/bus/input/devices ==="
echo ""

# Parse /proc/bus/input/devices for Sunshine-related devices
# The file format is: I: / N: / P: / S: / U: / H: / B: lines per device
awk '
BEGIN { device_count = 0 }
/^I:/ { 
    vendor = ""; product = ""; name = ""; phys = ""; handlers = ""
}
/^I:.*Vendor=/ { 
    match($0, /Vendor=([0-9a-f]+)/, v); vendor = v[1]
    match($0, /Product=([0-9a-f]+)/, p); product = p[1]
}
/^N:/ {
    match($0, /Name="([^"]+)"/, n); name = n[1]
}
/^P:/ {
    match($0, /Phys=(.*)/, ph); phys = ph[1]
}
/^H:/ {
    handlers = $0
}
/^$|^$/ {
    # Empty line = end of device record
    # Check if this is a Sunshine device
    is_sunshine = 0
    if (name ~ /^Sunshine/) is_sunshine = 1
    if (name ~ /passthrough/ && vendor == "bee f") is_sunshine = 1
    if (vendor == "beef" && product == "dead") is_sunshine = 1
    
    if (is_sunshine && name != "") {
        printf "  Device: %s\n", name
        printf "    Vendor/Product: %s:%s\n", vendor, product
        printf "    Phys: %s\n", phys
        # Extract event number from handlers
        if (match(handlers, /event([0-9]+)/, e)) {
            printf "    Event: /dev/input/event%s\n", e[1]
            printf "    SysFS: /sys/class/input/event%s/device/name\n", e[1]
        }
        printf "\n"
        device_count++
    }
    vendor = ""; product = ""; name = ""; phys = ""; handlers = ""
}
END {
    printf "Total Sunshine virtual devices found: %d\n", device_count
}
' /proc/bus/input/devices

echo ""
echo "=== Checking /sys/class/input/ for virtual devices ==="
echo ""

for event_dir in /sys/class/input/event*/; do
    if [ -f "${event_dir}device/name" ]; then
        name=$(cat "${event_dir}device/name" 2>/dev/null)
        if echo "${name}" | grep -qi "sunshine\|passthrough"; then
            event_num=$(basename "${event_dir}")
            phys=$(cat "${event_dir}device/phys" 2>/dev/null || echo "N/A")
            echo "  ${event_num}: name='${name}' phys='${phys}'"
        fi
    fi
done

echo ""
echo "=== CouchPlay VirtualDeviceWatcher Integration ==="
echo ""
echo "The existing VirtualDeviceWatcher::isVirtualDevice() checks:"
echo "  1. Name contains 'virtual', 'xtest', or 'uinput'"
echo "  2. Phys path is empty or contains 'virtual'"
echo ""
echo "To detect Sunshine devices, add these checks:"
echo "  3. Name starts with 'Sunshine ' (gamepads)"
echo "  4. Name contains 'passthrough' (mouse/keyboard/touch/pen)"
echo "  5. Vendor ID is 0xBEEF with Product ID 0xDEAD"
echo ""
echo "Proposed isVirtualDevice() extension:"
echo '  if (lowerName.startsWith("sunshine")) return true;'
echo '  if (lowerName.contains("passthrough")) return true;'
echo '  // Check vendor/product via sysfs if needed'
