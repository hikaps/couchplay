#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Launch multiple Sunshine instances with different ports.
# Validates that Sunshine can run as multi-instance subprocess.
#
# Prerequisites:
#   - Sunshine must be installed (sunshine binary in PATH)
#   - This script should be run with appropriate permissions
#
# Usage: ./02-launch-multi-instance.sh [num_instances] [base_port]
#   num_instances: number of instances to launch (default: 2)
#   base_port:     starting base port (default: 47989)

set -euo pipefail

NUM_INSTANCES="${1:-2}"
BASE_PORT="${2:-47989}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== Sunshine Multi-Instance Validation ==="
echo "Instances: ${NUM_INSTANCES}"
echo "Base port: ${BASE_PORT}"
echo ""

# Check Sunshine is installed
if ! command -v sunshine &>/dev/null; then
    echo "ERROR: Sunshine is not installed or not in PATH."
    echo "Install from: https://github.com/LizardByte/Sunshine/releases"
    echo ""
    echo "On Bazzite/Fedora:"
    echo "  sudo dnf install sunshine"
    echo "  or: flatpak install flathub dev.lizardbyte.app.Sunshine"
    echo ""
    echo "For this spike, you can also use a container:"
    echo "  docker run -it lizardbyte/sunshine:latest --help"
    exit 1
fi

echo "Sunshine version: $(sunshine --version 2>/dev/null || echo 'unknown')"
echo ""

# Generate configs for all instances
PIDS=()
CONFIG_DIRS=()

cleanup() {
    echo ""
    echo "=== Cleaning up ${#PIDS[@]} Sunshine instances ==="
    for pid in "${PIDS[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            echo "Killing Sunshine PID ${pid}..."
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    # Wait for processes to terminate
    sleep 2
    for pid in "${PIDS[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            echo "Force killing Sunshine PID ${pid}..."
            kill -KILL "${pid}" 2>/dev/null || true
        fi
    done
    # Clean up config dirs
    for dir in "${CONFIG_DIRS[@]}"; do
        echo "Removing config dir: ${dir}"
        rm -rf "${dir}"
    done
    echo "Cleanup complete."
}
trap cleanup EXIT

for i in $(seq 0 $((NUM_INSTANCES - 1))); do
    # Generate config using the config generation script
    PORT_OFFSET=$((i * 30))
    ACTUAL_PORT=$((BASE_PORT + PORT_OFFSET))
    
    "${SCRIPT_DIR}/01-generate-sunshine-config.sh" "${i}" "${BASE_PORT}"
    
    CONFIG_DIR="/tmp/couchplay-sunshine-${i}"
    CONFIG_DIRS+=("${CONFIG_DIR}")
    
    echo ""
    echo "=== Launching Sunshine instance ${i} on port ${ACTUAL_PORT} ==="
    
    # Launch Sunshine with custom config
    sunshine "${CONFIG_DIR}/sunshine.conf" &
    PID=$!
    PIDS+=("${PID}")
    
    echo "Instance ${i} started with PID ${PID}"
    echo "  Config:  ${CONFIG_DIR}/sunshine.conf"
    echo "  Port:    ${ACTUAL_PORT}"
    echo "  Web UI:  https://localhost:$((ACTUAL_PORT + 1))"
    echo ""
    
    # Small delay between launches to avoid resource contention
    sleep 2
done

echo "=== All ${NUM_INSTANCES} Sunshine instances launched ==="
echo ""
echo "PIDs: ${PIDS[*]}"
echo ""

# Verify all instances are running
echo "=== Verification ==="
ALL_RUNNING=true
for i in "${!PIDS[@]}"; do
    PID="${PIDS[${i}]}"
    if kill -0 "${PID}" 2>/dev/null; then
        echo "Instance ${i} (PID ${PID}): RUNNING"
    else
        echo "Instance ${i} (PID ${PID}): NOT RUNNING"
        ALL_RUNNING=false
    fi
done

echo ""

# Check port bindings
echo "=== Port bindings ==="
for i in $(seq 0 $((NUM_INSTANCES - 1))); do
    PORT_OFFSET=$((i * 30))
    ACTUAL_PORT=$((BASE_PORT + PORT_OFFSET))
    echo "Instance ${i}:"
    ss -tlnp 2>/dev/null | grep -E "${ACTUAL_PORT}|$((ACTUAL_PORT + 1))|$((ACTUAL_PORT + 21))" || echo "  No ports found"
    echo ""
done

if [ "${ALL_RUNNING}" = true ]; then
    echo "=== RESULT: ALL INSTANCES RUNNING ==="
    echo "Multi-instance validation: PASS"
else
    echo "=== RESULT: SOME INSTANCES FAILED ==="
    echo "Multi-instance validation: PARTIAL"
fi

echo ""
echo "Press Ctrl+C to stop all instances and clean up."
echo "Connect Moonlight clients to different ports to test streaming."

# Wait for all processes
wait
