#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 CouchPlay Contributors

set -euo pipefail

MAX_DOCUMENT_SIZE=$((16 * 1024 * 1024))
STEAM_APP_ID="com.valvesoftware.Steam"

fail() {
    printf '%s\n' "$2" >&2
    return "${1:-1}"
}

require_user() {
    [[ "$(id -u)" -ne 0 ]] || fail 2 "root-user-operation-refused"
}

home_dir() {
    local home=${HOME:-}
    [[ -n "$home" && "$home" == /* ]] || fail 2 "invalid-home"
    printf '%s\n' "$home"
}

is_game_mode() {
    [[ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" || -n "${SteamGamepadUI:-}" || "${XDG_CURRENT_DESKTOP:-}" == "gamescope" ]]
}

canonical_dir() {
    local candidate=$1
    [[ -d "$candidate" && -f "$candidate/config/libraryfolders.vdf" ]] || return 1
    realpath -e -- "$candidate"
}

steam_roots() {
    local home root canonical
    home=$(home_dir)
    local candidates=(
        "$home/.steam/steam"
        "$home/.local/share/Steam"
        "$home/.var/app/$STEAM_APP_ID/.steam/steam"
        "$home/.var/app/$STEAM_APP_ID/.local/share/Steam"
        "$home/.var/app/$STEAM_APP_ID/data/Steam"
    )
    local -A seen=()
    for root in "${candidates[@]}"; do
        if canonical=$(canonical_dir "$root" 2>/dev/null) && [[ -z "${seen[$canonical]:-}" ]]; then
            seen["$canonical"]=1
            printf '%s\n' "$canonical"
        fi
    done
}

root_kind() {
    [[ "$1" == *"/.var/app/$STEAM_APP_ID/"* ]] && printf '%s\n' flatpak || printf '%s\n' native
}

root_valid() {
    local expected=$1 root canonical
    while IFS= read -r root; do
        canonical=$(realpath -e -- "$root")
        [[ "$canonical" == "$expected" ]] && return 0
    done < <(steam_roots)
    return 1
}

account_valid() {
    local root=$1 account=$2
    [[ "$account" =~ ^[1-9][0-9]*$ ]] || return 1
    root_valid "$root" || return 1
    local userdata="$root/userdata" account_dir="$root/userdata/$account" config="$root/userdata/$account/config"
    [[ ! -L "$userdata" && ! -L "$account_dir" && ! -L "$config" ]] || return 1
    [[ -d "$config" && "$(realpath -e -- "$config")" == "$config" ]]
}

steam_binary() {
    local root=$1
    local candidate
    for candidate in "$root/steam.sh" "$root/ubuntu12_64/steam" "$root/ubuntu12_32/steam"; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

steam_running() {
    local root=$1 kind pid exe candidate canonical uid
    local -a native_binaries=()
    kind=$(root_kind "$root")
    uid=$(id -u)
    if [[ "$kind" == flatpak ]]; then
        if ! command -v flatpak >/dev/null 2>&1; then
            return 1
        fi
        while IFS= read -r pid; do
            [[ -r "/proc/$pid/status" ]] || continue
            [[ "$(awk '/^Uid:/{print $2; exit}' "/proc/$pid/status" 2>/dev/null)" == "$uid" ]] && return 0
        done < <(flatpak ps --columns=application,pid 2>/dev/null | awk -v app="$STEAM_APP_ID" '$1 == app {print $2}')
        return 1
    fi

    for candidate in "$root/ubuntu12_64/steam" "$root/ubuntu12_32/steam"; do
        [[ -x "$candidate" ]] || continue
        canonical=$(readlink -f -- "$candidate" 2>/dev/null || true)
        [[ -n "$canonical" ]] && native_binaries+=("$canonical")
    done
    ((${#native_binaries[@]})) || return 1

    for pid in /proc/[0-9]*; do
        pid=${pid##*/}
        [[ -r "/proc/$pid/status" ]] || continue
        [[ "$(awk '/^Uid:/{print $2; exit}' "/proc/$pid/status" 2>/dev/null)" == "$uid" ]] || continue
        exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
        for candidate in "${native_binaries[@]}"; do
            [[ "$exe" == "$candidate" ]] && return 0
        done
    done
    return 1
}

emit_field() {
    printf '%s\0' "$1"
}

probe() {
    require_user
    local home data_home root account kind running uid
    home=$(home_dir)
    data_home=${XDG_DATA_HOME:-$home/.local/share}
    [[ "$data_home" == /* ]] || fail 2 "invalid-data-home"
    uid=$(id -u)
    emit_field H
    emit_field "$home"
    emit_field "$uid"
    emit_field "$data_home"
    if is_game_mode; then emit_field G; emit_field 1; else emit_field G; emit_field 0; fi
    while IFS= read -r root; do
        kind=$(root_kind "$root")
        if steam_running "$root"; then running=1; else running=0; fi
        emit_field I
        emit_field "$root"
        emit_field "$kind"
        emit_field "${root}/$(basename "$(steam_binary "$root" 2>/dev/null || printf 'steam')")"
        emit_field "$running"
        while IFS= read -r account; do
            [[ -d "$root/userdata/$account/config" ]] || continue
            emit_field A
            emit_field "$root"
            emit_field "$account"
        done < <(find "$root/userdata" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | awk '/^[1-9][0-9]*$/' | sort -n)
    done < <(steam_roots)
}

read_shortcuts() {
    require_user
    local root=$1 account=$2 path
    account_valid "$root" "$account" || fail 3 "invalid-account"
    path="$root/userdata/$account/config/shortcuts.vdf"
    [[ -e "$path" ]] || return 3
    [[ -f "$path" && ! -L "$path" && "$(stat -c '%u' -- "$path")" == "$(id -u)" ]] || fail 3 "unsafe-shortcuts-file"
    [[ "$(stat -c '%s' -- "$path")" -le "$MAX_DOCUMENT_SIZE" ]] || fail 3 "shortcuts-file-too-large"
    cat -- "$path"
}

assert_expected() {
    local path=$1 expected=$2 actual
    if [[ "$expected" == missing ]]; then
        [[ ! -e "$path" ]] || fail 4 "shortcuts-conflict"
        return
    fi
    [[ -f "$path" && ! -L "$path" ]] || fail 4 "shortcuts-conflict"
    actual=$(sha256sum -- "$path" | awk '{print $1}')
    [[ "$actual" == "$expected" ]] || fail 4 "shortcuts-conflict"
}

commit_shortcuts() {
    require_user
    local root=$1 account=$2 expected=$3 path config config_fd config_fd_identity config_path_identity temp backup_temp mode
    account_valid "$root" "$account" || fail 3 "invalid-account"
    config="$root/userdata/$account/config"
    exec 9<"$config" || fail 3 "unsafe-account-path"
    config_fd="/proc/$BASHPID/fd/9"
    config_fd_identity=$(stat -Lc '%d:%i' -- "$config_fd") || fail 4 "unsafe-account-path"
    config_path_identity=$(stat -c '%d:%i' -- "$config") || fail 4 "unsafe-account-path"
    [[ "$config_fd_identity" == "$config_path_identity" ]] || fail 4 "unsafe-account-path"
    flock -n 9 || fail 4 "shortcuts-busy"
    account_valid "$root" "$account" || fail 4 "unsafe-account-path"
    config_path_identity=$(stat -c '%d:%i' -- "$config") || fail 4 "unsafe-account-path"
    [[ "$config_fd_identity" == "$config_path_identity" ]] || fail 4 "unsafe-account-path"

    path="$config_fd/shortcuts.vdf"
    if [[ -e "$path" || -L "$path" ]]; then
        [[ -f "$path" && ! -L "$path" && "$(stat -c '%u' -- "$path")" == "$(id -u)" && "$(stat -c '%h' -- "$path")" == 1 ]] || fail 4 "unsafe-shortcuts-file"
    fi
    steam_running "$root" && fail 4 "steam-still-running"
    assert_expected "$path" "$expected"
    temp=$(mktemp "$config_fd/.shortcuts.vdf.couchplay.XXXXXX")
    trap 'rm -f -- "${temp:-}" "${backup_temp:-}"' RETURN
    cat >"$temp"
    [[ "$(stat -c '%s' -- "$temp")" -le "$MAX_DOCUMENT_SIZE" ]] || fail 4 "shortcuts-file-too-large"
    chmod 600 "$temp"
    if [[ -e "$path" ]]; then
        mode=$(stat -c '%a' -- "$path")
        backup_temp=$(mktemp "$config_fd/.shortcuts.vdf.couchplay-backup.XXXXXX")
        cp -- "$path" "$backup_temp"
        chmod 600 "$backup_temp"
        mv -fT -- "$backup_temp" "$config_fd/shortcuts.vdf.couchplay-backup"
        chmod "$mode" "$temp"
    fi
    account_valid "$root" "$account" || fail 4 "unsafe-account-path"
    config_path_identity=$(stat -c '%d:%i' -- "$config") || fail 4 "unsafe-account-path"
    [[ "$config_fd_identity" == "$config_path_identity" ]] || fail 4 "unsafe-account-path"
    steam_running "$root" && fail 4 "steam-started-during-write"
    mv -fT -- "$temp" "$path"
    trap - RETURN
    printf '%s\n' committed
}

export_payload() {
    require_user
    local identity=$1 kind=$2 home data_dir canonical_data_dir app_dir app_dir_fd app_fd_identity app_path_identity
    local output_dir output_dir_fd output_fd_identity output_path_identity output_owner output target temp uid
    [[ "$identity" =~ ^[0-9a-f]{64}$ ]] || fail 2 "invalid-profile-identity"
    case "$kind" in
        launcher) output="$identity.sh" ;;
        gamemode) output=couchplay-gamemode ;;
        icon) output=couchplay.png ;;
        *) fail 2 "invalid-export-kind" ;;
    esac
    home=$(home_dir)
    data_dir=${XDG_DATA_HOME:-$home/.local/share}
    [[ "$data_dir" == /* ]] || fail 2 "invalid-data-home"
    mkdir -p -- "$data_dir" || fail 3 "unsafe-export-directory"
    canonical_data_dir=$(realpath -e -- "$data_dir") || fail 3 "unsafe-export-directory"
    uid=$(id -u)
    [[ -d "$canonical_data_dir" && "$(stat -c '%u' -- "$canonical_data_dir")" == "$uid" ]] || fail 3 "unsafe-export-directory"

    app_dir="$canonical_data_dir/couchplay"
    if [[ ! -e "$app_dir" && ! -L "$app_dir" ]]; then
        mkdir -m 700 -- "$app_dir" 2>/dev/null || true
    fi
    [[ -d "$app_dir" && ! -L "$app_dir" && "$(realpath -e -- "$app_dir")" == "$app_dir" ]] || fail 3 "unsafe-export-directory"
    exec 8<"$app_dir" || fail 3 "unsafe-export-directory"
    app_dir_fd="/proc/$BASHPID/fd/8"
    app_fd_identity=$(stat -Lc '%d:%i' -- "$app_dir_fd") || fail 3 "unsafe-export-directory"
    app_path_identity=$(stat -c '%d:%i' -- "$app_dir") || fail 3 "unsafe-export-directory"
    [[ "$app_fd_identity" == "$app_path_identity" && "$(stat -Lc '%u' -- "$app_dir_fd")" == "$uid" ]] || fail 3 "unsafe-export-directory"
    chmod 700 "$app_dir_fd" || fail 3 "unsafe-export-directory"

    output_dir="$app_dir_fd/steam-shortcuts"
    if [[ ! -e "$output_dir" && ! -L "$output_dir" ]]; then
        mkdir -m 700 -- "$output_dir" 2>/dev/null || true
    fi
    [[ -d "$output_dir" && ! -L "$output_dir" \
        && "$(realpath -e -- "$output_dir")" == "$canonical_data_dir/couchplay/steam-shortcuts" ]] \
        || fail 3 "unsafe-export-directory"
    exec 9<"$output_dir" || fail 3 "unsafe-export-directory"
    output_dir_fd="/proc/$BASHPID/fd/9"
    output_fd_identity=$(stat -Lc '%d:%i' -- "$output_dir_fd") || fail 3 "unsafe-export-directory"
    output_path_identity=$(stat -c '%d:%i' -- "$output_dir") || fail 3 "unsafe-export-directory"
    output_owner=$(stat -Lc '%u' -- "$output_dir_fd") || fail 3 "unsafe-export-directory"
    [[ "$output_fd_identity" == "$output_path_identity" && "$output_owner" == "$uid" ]] || fail 3 "unsafe-export-directory"
    chmod 700 "$output_dir_fd" || fail 3 "unsafe-export-directory"

    target="$output_dir_fd/$output"
    if [[ -e "$target" || -L "$target" ]]; then
        [[ -f "$target" && ! -L "$target" ]] || fail 3 "unsafe-export-target"
    fi
    temp=$(mktemp "$output_dir_fd/.export.XXXXXX")
    trap 'rm -f -- "$temp"' RETURN
    cat >"$temp"
    if [[ "$kind" == icon ]]; then chmod 600 "$temp"; else chmod 700 "$temp"; fi
    app_path_identity=$(stat -c '%d:%i' -- "$app_dir") || fail 3 "unsafe-export-directory"
    output_path_identity=$(stat -c '%d:%i' -- "$output_dir") || fail 3 "unsafe-export-directory"
    [[ "$app_fd_identity" == "$app_path_identity" && "$output_fd_identity" == "$output_path_identity" ]] \
        || fail 3 "unsafe-export-directory"
    mv -fT -- "$temp" "$target"
    trap - RETURN
    printf '%s\n' "$canonical_data_dir/couchplay/steam-shortcuts/$output"
}

shutdown_steam() {
    require_user
    local root=$1 binary
    is_game_mode && fail 2 "game-mode-restart-refused"
    root_valid "$root" || fail 3 "invalid-steam-root"
    if [[ "$(root_kind "$root")" == native ]]; then
        binary=$(steam_binary "$root") || fail 3 "steam-binary-not-found"
        "$binary" -shutdown >/dev/null 2>&1 || true
    else
        flatpak run "$STEAM_APP_ID" -shutdown >/dev/null 2>&1 || true
    fi
}

start_steam() {
    require_user
    local root=$1 binary
    is_game_mode && fail 2 "game-mode-restart-refused"
    root_valid "$root" || fail 3 "invalid-steam-root"
    if [[ "$(root_kind "$root")" == native ]]; then
        binary=$(steam_binary "$root") || fail 3 "steam-binary-not-found"
        nohup "$binary" >/dev/null 2>&1 </dev/null &
    else
        nohup flatpak run "$STEAM_APP_ID" >/dev/null 2>&1 </dev/null &
    fi
    printf '%s\n' started
}

flatpak_steam_preflight() {
    require_user
    flatpak run --command=flatpak-spawn "$STEAM_APP_ID" --host /usr/bin/true >/dev/null 2>&1
}

main() {
    local operation=${1:-}
    shift || true
    case "$operation" in
        probe) probe "$@" ;;
        read) read_shortcuts "$@" ;;
        commit) commit_shortcuts "$@" ;;
        export) export_payload "$@" ;;
        shutdown) shutdown_steam "$@" ;;
        start) start_steam "$@" ;;
        flatpak-steam-preflight) flatpak_steam_preflight "$@" ;;
        *) fail 2 "unknown-operation" ;;
    esac
}

if [[ ${BASH_SOURCE[0]:-} == "$0" || "$0" == couchplay-steam-host ]]; then
    main "$@"
fi
