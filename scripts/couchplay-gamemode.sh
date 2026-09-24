#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors
#
# CouchPlay Game Mode Launcher
#
# Launches CouchPlay inside SteamOS Game Mode by starting a nested KWin Wayland
# compositor. It is also safe to use as a normal Desktop Mode launcher.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROUTE=auto
NATIVE_BIN=

if [[ "${1:-}" == "--couchplay-native" ]]; then
    [[ $# -ge 3 && "$3" == "--" ]] || { echo "Error: --couchplay-native requires an executable and --." >&2; exit 2; }
    ROUTE=native
    NATIVE_BIN=$2
    shift 3
elif [[ "${1:-}" == "--couchplay-flatpak" ]]; then
    [[ "${2:-}" == "--" ]] || { echo "Error: --couchplay-flatpak requires --." >&2; exit 2; }
    ROUTE=flatpak
    shift 2
fi

if [[ "$ROUTE" == native ]]; then
    [[ -x "$NATIVE_BIN" ]] || { echo "Error: CouchPlay binary is not executable: $NATIVE_BIN" >&2; exit 1; }
    COUCHPLAY_BIN=$NATIVE_BIN
elif [[ "$ROUTE" == auto ]]; then
    if [ -x "$SCRIPT_DIR/../build/bin/couchplay" ]; then
        COUCHPLAY_BIN="$SCRIPT_DIR/../build/bin/couchplay"
    elif command -v couchplay &>/dev/null; then
        COUCHPLAY_BIN="$(command -v couchplay)"
    elif [ -x /usr/local/bin/couchplay ]; then
        COUCHPLAY_BIN="/usr/local/bin/couchplay"
    else
        COUCHPLAY_BIN=
    fi
fi

is_game_mode() {
    if [ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" ]; then
        return 0
    fi
    if [ -n "${SteamGamepadUI:-}" ] || [ "${XDG_CURRENT_DESKTOP:-}" = "gamescope" ]; then
        return 0
    fi
    return 1
}

KWIN_PID=""
COUCHPLAY_PID=""
cleanup() {
    echo "CouchPlay Game Mode: Cleaning up..."
    if [ -n "$COUCHPLAY_PID" ] && kill -0 "$COUCHPLAY_PID" 2>/dev/null; then
        kill "$COUCHPLAY_PID" 2>/dev/null || true
        wait "$COUCHPLAY_PID" 2>/dev/null || true
        COUCHPLAY_PID=""
    fi
    if [ -n "$KWIN_PID" ] && kill -0 "$KWIN_PID" 2>/dev/null; then
        kill "$KWIN_PID" 2>/dev/null || true
        wait "$KWIN_PID" 2>/dev/null || true
        KWIN_PID=""
    fi
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

run_couchplay() {
    if [[ "$ROUTE" == flatpak ]]; then
        if [[ "${COUCHPLAY_HOST_SPAWN:-0}" == 1 ]]; then
            flatpak-spawn --host /usr/bin/flatpak run \
                --env=WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
                --env=QT_QPA_PLATFORM="$QT_QPA_PLATFORM" \
                io.github.hikaps.couchplay "$@"
        else
            flatpak run io.github.hikaps.couchplay "$@"
        fi
    else
        "$COUCHPLAY_BIN" "$@"
    fi
}

run_couchplay_forwarded() {
    if [[ "$ROUTE" == flatpak ]]; then
        if [[ "${COUCHPLAY_HOST_SPAWN:-0}" == 1 ]]; then
            ( exec flatpak-spawn --host /usr/bin/flatpak run --env=WAYLAND_DISPLAY="$WAYLAND_DISPLAY" --env=QT_QPA_PLATFORM="$QT_QPA_PLATFORM" io.github.hikaps.couchplay "$@" ) &
        else
            ( exec flatpak run io.github.hikaps.couchplay "$@" ) &
        fi
    else
        ( exec "$COUCHPLAY_BIN" "$@" ) &
    fi
    COUCHPLAY_PID=$!
    local status
    if wait "$COUCHPLAY_PID"; then
        status=0
    else
        status=$?
    fi
    COUCHPLAY_PID=""
    return "$status"
}
if is_game_mode; then
    echo "Detected: SteamOS Game Mode (gamescope session)"
else
    echo "Detected: Desktop Mode"
    echo "Launching CouchPlay directly..."
    if run_couchplay_forwarded "$@"; then
        exit 0
    else
        exit $?
    fi
fi

if [[ "$ROUTE" == flatpak && -z "$(command -v kwin_wayland 2>/dev/null || true)" ]]; then
    if command -v flatpak-spawn >/dev/null 2>&1; then
        KWIN_COMMAND=(flatpak-spawn --host --watch-bus kwin_wayland)
        COUCHPLAY_HOST_SPAWN=1
    else
        echo "Error: kwin_wayland is unavailable in the Flatpak and flatpak-spawn is unavailable." >&2
        exit 1
    fi
else
    command -v kwin_wayland >/dev/null 2>&1 || { echo "Error: kwin_wayland not found." >&2; exit 1; }
    KWIN_COMMAND=("$(command -v kwin_wayland)")
    COUCHPLAY_HOST_SPAWN=0
fi

CP_WAYLAND_DISPLAY="couchplay-$$"
export CP_WAYLAND_DISPLAY

echo "Starting nested KWin Wayland compositor..."
"${KWIN_COMMAND[@]}" \
    --no-lockscreen \
    --no-global-shortcuts \
    --wayland-display "$CP_WAYLAND_DISPLAY" \
    --width "${GAMESCOPE_WIDTH:-1920}" \
    --height "${GAMESCOPE_HEIGHT:-1080}" &
KWIN_PID=$!

echo "Waiting for KWin D-Bus interface..."
KWIN_READY=false
for _ in $(seq 1 20); do
    if dbus-send --session --dest=org.kde.KWin --print-reply \
        /KWin org.kde.KWin.currentDesktop &>/dev/null 2>&1; then
        KWIN_READY=true
        break
    fi
    sleep 0.5
done
if [ "$KWIN_READY" = false ]; then
    echo "Error: KWin did not start within 10 seconds." >&2
    exit 1
fi

SOCKET_READY=false
for _ in $(seq 1 20); do
    if [ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/${CP_WAYLAND_DISPLAY}" ]; then
        SOCKET_READY=true
        break
    fi
    sleep 0.5
done
if [ "$SOCKET_READY" = false ]; then
    echo "Error: kwin Wayland socket ${CP_WAYLAND_DISPLAY} did not appear." >&2
    exit 1
fi

echo "KWin is ready (PID: $KWIN_PID)"
export WAYLAND_DISPLAY="$CP_WAYLAND_DISPLAY"
export QT_QPA_PLATFORM=wayland
export QT_LOGGING_RULES="couchplay.*=true"
export QT_MESSAGE_PATTERN="[%{time hh:mm:ss.zzz}] %{if-category}%{category}: %{endif}%{message}"

if run_couchplay "$@"; then
    COUCHPLAY_EXIT=0
else
    COUCHPLAY_EXIT=$?
fi
echo "CouchPlay exited with code $COUCHPLAY_EXIT"
exit "$COUCHPLAY_EXIT"
