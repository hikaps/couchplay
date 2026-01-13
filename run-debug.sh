#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# Run CouchPlay with debug logging enabled
#
# Usage:
#   ./run-debug.sh              # Enable all couchplay.* debug logs
#   ./run-debug.sh --all        # Enable ALL Qt debug logs (very verbose)
#   ./run-debug.sh --steam      # Enable only Steam-related logs
#   ./run-debug.sh --helper     # Enable only D-Bus helper logs

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$SCRIPT_DIR/build/bin/couchplay"

if [ ! -x "$BINARY" ]; then
    echo "Error: CouchPlay binary not found at $BINARY"
    echo "Run 'distrobox enter fedora-dev -- bash -c \"cd $SCRIPT_DIR && cmake --build build\"' first"
    exit 1
fi

# Default: enable all couchplay logging categories
LOGGING_RULES="couchplay.*=true"

# Parse arguments
case "${1:-}" in
    --all)
        # Enable ALL debug output (very verbose)
        LOGGING_RULES="*.debug=true"
        echo "Enabling ALL debug logs (very verbose)"
        ;;
    --steam)
        # Steam config sync only
        LOGGING_RULES="couchplay.steam=true"
        echo "Enabling Steam config logs only"
        ;;
    --helper)
        # D-Bus helper communication only
        LOGGING_RULES="couchplay.helper=true"
        echo "Enabling D-Bus helper logs only"
        ;;
    --core)
        # Core session management only
        LOGGING_RULES="couchplay.core=true"
        echo "Enabling core session logs only"
        ;;
    --gamescope)
        # Gamescope instance management only
        LOGGING_RULES="couchplay.gamescope=true"
        echo "Enabling gamescope logs only"
        ;;
    --sharing)
        # Directory sharing only
        LOGGING_RULES="couchplay.sharing=true"
        echo "Enabling directory sharing logs only"
        ;;
    "")
        echo "Enabling all CouchPlay debug logs"
        ;;
    *)
        echo "Unknown option: $1"
        echo "Usage: $0 [--all|--steam|--helper|--core|--gamescope|--sharing]"
        exit 1
        ;;
esac

# Set message format for better readability
export QT_MESSAGE_PATTERN="[%{time hh:mm:ss.zzz}] %{if-category}%{category}: %{endif}%{message}"

# Enable the logging rules
export QT_LOGGING_RULES="$LOGGING_RULES"

echo "QT_LOGGING_RULES=$LOGGING_RULES"
echo "Starting CouchPlay..."
echo "---"

exec "$BINARY" "${@:2}"
