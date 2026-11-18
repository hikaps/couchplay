#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 hikaps
#
# Migrate from the old CouchPlay root helper (one Linux account per player)
# to the rootless per-player sandboxes.
#
# Usage:
#   sudo ./scripts/migrate-from-helper.sh [--user DESKTOP_USER] [--flatpak] [--yes] [--help]
#
# Without --yes this only lists what would be imported (dry run).

set -euo pipefail

FLATPAK_ID="io.github.hikaps.couchplay"
GROUP_NAME="couchplay"
NAME_RE='^[a-z_][a-z0-9_-]{0,31}$'

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*" >&2; }
die() { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: sudo $0 [--user DESKTOP_USER] [--flatpak] [--players-dir DIR] [--yes] [--help]

Imports the home directories of old CouchPlay player accounts into the new
rootless player store and removes the old CouchPlay root helper.

  --user USER   Desktop user that runs CouchPlay (default: \$SUDO_USER)
  --flatpak     Use the Flatpak data directory for the player store
  --players-dir DIR  Import into DIR (for example on a larger drive) instead of the default store;
                set the same folder in CouchPlay under Players > Storage location
  --yes         Actually import and remove (without it, only a dry run)
  --help        Show this help
EOF
}

has_systemd() {
    [[ -d /run/systemd/system ]] && command -v systemctl >/dev/null 2>&1
}

DESKTOP_USER="${SUDO_USER:-}"
USE_FLATPAK=false
PLAYERS_DIR_OVERRIDE=""
ASSUME_YES=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --user)
            [[ $# -ge 2 ]] || die "--user needs a user name"
            DESKTOP_USER="$2"
            shift 2
            ;;
        --players-dir)
            [[ $# -ge 2 ]] || die "--players-dir needs a directory"
            PLAYERS_DIR_OVERRIDE="$2"
            shift 2
            ;;
        --flatpak) USE_FLATPAK=true; shift ;;
        --yes|-y) ASSUME_YES=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; die "Unknown option: $1" ;;
    esac
done

[[ "$(id -u)" -eq 0 ]] || die "This script must run as root. Use: sudo $0"
[[ -n "$DESKTOP_USER" ]] || die "Cannot tell which desktop user to migrate for. Pass --user NAME."
[[ "$DESKTOP_USER" != "root" ]] || die "The desktop user must not be root. Pass --user NAME (your normal login)."

passwd_entry="$(getent passwd "$DESKTOP_USER" || true)"
[[ -n "$passwd_entry" ]] || die "User '$DESKTOP_USER' does not exist."
DESKTOP_HOME="$(cut -d: -f6 <<<"$passwd_entry")"
DESKTOP_GID="$(cut -d: -f4 <<<"$passwd_entry")"
DESKTOP_GROUP="$(getent group "$DESKTOP_GID" | cut -d: -f1 || true)"
DESKTOP_GROUP="${DESKTOP_GROUP:-$DESKTOP_USER}"
[[ -d "$DESKTOP_HOME" ]] || die "Home directory of '$DESKTOP_USER' ($DESKTOP_HOME) does not exist."

if [[ -n "$PLAYERS_DIR_OVERRIDE" ]]; then
    [[ "$PLAYERS_DIR_OVERRIDE" == /* ]] || die "--players-dir needs an absolute path"
    PLAYERS_ROOT="${PLAYERS_DIR_OVERRIDE%/}"
elif [[ "$USE_FLATPAK" == "true" ]]; then
    PLAYERS_ROOT="$DESKTOP_HOME/.var/app/$FLATPAK_ID/data/hikaps/CouchPlay/players"
else
    PLAYERS_ROOT="$DESKTOP_HOME/.local/share/couchplay/players"
fi

# --- Discover old players ---------------------------------------------------

PLAYERS=()

add_candidate() {
    local name="$1" entry uid home existing
    [[ "$name" =~ $NAME_RE ]] || return 0
    entry="$(getent passwd "$name" || true)"
    [[ -n "$entry" ]] || return 0
    uid="$(cut -d: -f3 <<<"$entry")"
    home="$(cut -d: -f6 <<<"$entry")"
    [[ "$uid" =~ ^[0-9]+$ && "$uid" -ge 1000 ]] || return 0
    [[ -d "$home" ]] || return 0
    [[ "$name" != "$DESKTOP_USER" ]] || return 0
    for existing in "${PLAYERS[@]+"${PLAYERS[@]}"}"; do
        [[ "$existing" != "$name" ]] || return 0
    done
    PLAYERS+=("$name")
}

group_entry="$(getent group "$GROUP_NAME" || true)"
if [[ -n "$group_entry" ]]; then
    group_gid="$(cut -d: -f3 <<<"$group_entry")"
    group_members="$(cut -d: -f4 <<<"$group_entry")"
    IFS=',' read -r -a member_list <<<"$group_members"
    for member in "${member_list[@]+"${member_list[@]}"}"; do
        [[ -n "$member" ]] && add_candidate "$member"
    done
    while IFS=: read -r pname _ _ pgid _; do
        if [[ "$pgid" == "$group_gid" ]]; then
            add_candidate "$pname"
        fi
    done < <(getent passwd)
else
    info "Group '$GROUP_NAME' not found; no old player accounts to import."
fi

home_of() { getent passwd "$1" | cut -d: -f6; }

info "Desktop user:  $DESKTOP_USER"
info "Player store:  $PLAYERS_ROOT"
echo
if [[ ${#PLAYERS[@]} -eq 0 ]]; then
    info "No old CouchPlay player accounts found."
else
    info "Old players that can be imported:"
    for name in "${PLAYERS[@]}"; do
        home="$(home_of "$name")"
        size="$(du -sh --exclude=.cache "$home" 2>/dev/null | cut -f1 || true)"
        printf '  - %s  (%s, %s without .cache)\n' "$name" "$home" "${size:-unknown size}"
    done
fi
echo

if [[ "$ASSUME_YES" != "true" ]]; then
    info "Dry run only. With --yes this script will:"
    echo "  1. copy each old player's home into $PLAYERS_ROOT/<name>/home"
    echo "  2. stop and remove the old CouchPlay helper service, polkit/dbus/pipewire files"
    echo "  3. remove the SteamOS system extension image, if present"
    echo "Linux accounts and /var/lib/couchplay are NOT touched."
    echo
    info "Re-run with --yes to proceed."
    exit 0
fi

# --- Import homes -------------------------------------------------------------

as_user() { runuser -u "$DESKTOP_USER" -- "$@"; }

# The old accounts' homes were /home/<name> (or /var/home/<name> on ostree systems). Inside the sandbox the
# player's home is the desktop user's home, and the old locations are unreadable there: Steam, its runtime and
# Heroic fail on absolute symlinks and config paths that still point at them. Rewrites them in a copied home.
# usage: relocate <home copy> <old name> <old home> <new home>
RELOCATE_HOME='
set -u
copy=$1; name=$2; old=$3; new=$4
prefixes=("$old" "/home/$name" "/var/home/$name")
while IFS= read -r -d "" link; do
    target=$(readlink -- "$link")
    for p in "${prefixes[@]}"; do
        if [[ $target == "$p" || $target == "$p"/* ]]; then
            ln -sfn -- "$new${target#"$p"}" "$link"
            break
        fi
    done
done < <(find "$copy" -xdev -type l \( -lname "/home/$name*" -o -lname "/var/home/$name*" -o -lname "$old*" \) -print0)
args=()
for p in "${prefixes[@]}"; do args+=(-e "$p"); done
grep -rIlZ --exclude-dir=steamapps --exclude-dir=logs --exclude-dir=.couchplay --exclude-dir=depotcache \
    --exclude-dir=htmlcache --exclude-dir=webhelper --exclude-dir=appcache --exclude="LOG*" --exclude="*.log" \
    "${args[@]}" "$copy" 2>/dev/null |
    xargs -0 -r sed -E -i "s#(/var)?/home/$name([^A-Za-z0-9_-]|\$)#$new\\2#g; s#$old([^A-Za-z0-9_-]|\$)#$new\\1#g"
exit 0
'
failed=false
for name in "${PLAYERS[@]+"${PLAYERS[@]}"}"; do
    src="$(home_of "$name")"
    dest="$PLAYERS_ROOT/$name"
    if [[ -d "$dest/home" ]] && [[ -n "$(ls -A "$dest/home" 2>/dev/null || true)" ]]; then
        info "Skipping '$name': $dest/home already exists and is not empty."
        continue
    fi
    info "Importing '$name' from $src ..."
    # Everything is created and extracted as the desktop user, so a symlink planted in the
    # player store can never make root change or write outside it.
    # Extracted into a staging directory and renamed only after the whole copy succeeded, so an
    # interrupted run can never leave a half-imported home that the next run would accept.
    staging="$dest/.home-import"
    as_user install -d -m 0700 "$PLAYERS_ROOT" "$dest"
    as_user rm -rf "$staging"
    as_user install -d -m 0700 "$staging"
    if tar -C "$src" --exclude=./.cache -cf - . | as_user tar -C "$staging" -xpf - --no-same-owner \
        && as_user bash -c "$RELOCATE_HOME" relocate "$staging" "$name" "$src" "$DESKTOP_HOME" \
        && { [[ ! -d "$dest/home" ]] || as_user rmdir "$dest/home"; } \
        && as_user mv "$staging" "$dest/home"; then
        info "Imported '$name'."
    else
        warn "Copying '$name' failed; removing the partial copy."
        # Only the staging directory: an existing layers/ directory is the player's data
        as_user rm -rf "$staging"
        failed=true
    fi
done

if [[ "$failed" == "true" ]]; then
    die "Some players could not be copied; the helper was NOT removed. Fix the error above and re-run."
fi

if [[ ${#PLAYERS[@]} -gt 0 ]]; then
    info "Absolute paths to the old homes were rewritten to $DESKTOP_HOME (the player's home inside the sandbox)."
    warn "If a player's Steam still fails to start, delete $PLAYERS_ROOT/<name>/home for a fresh sign-in."
fi

# --- Remove the helper (everything the old 'install-helper.sh uninstall' did) --

info "Uninstalling CouchPlay helper daemon..."

info "Stopping service..."
systemctl stop couchplay-helper.service 2>/dev/null || true
systemctl disable couchplay-helper.service 2>/dev/null || true

# --- Remove the SteamOS sysext image (as deployed by install.sh) --------------

info "Removing SteamOS system extension image (if any)..."
sysext_found=false
rm_sysext() {
    if [[ -e "$1" || -L "$1" ]]; then
        sysext_found=true
        rm -f "$1"
    fi
}
rm_sysext /var/lib/extensions/couchplay.steamos.raw
rm_sysext /var/lib/extensions/couchplay.raw
rm_sysext "$DESKTOP_HOME/.couchplay.steamos.raw"
rm_sysext "$DESKTOP_HOME/.couchplay.raw"
if [[ -d "$DESKTOP_HOME/.couchplay-extension" ]]; then
    sysext_found=true
    rm -rf "$DESKTOP_HOME/.couchplay-extension"
fi
if [[ "$sysext_found" == "true" ]] && command -v systemd-sysext >/dev/null 2>&1; then
    systemd-sysext refresh || warn "systemd-sysext refresh failed; a reboot will finish the removal."
fi

info "Removing installed files..."
# The old installer defaulted to /usr/local; packaged/sysext builds used /usr.
# Note: couchplay-vdisplay is NOT removed; the new version installs it in the same place.
# These paths are read-only while the SteamOS sysext is active; it is removed above first.
for prefix in /usr/local /usr; do
    rm -f "$prefix/libexec/couchplay-helper" || warn "Could not remove $prefix/libexec/couchplay-helper"
    rm -f "$prefix/share/dbus-1/system-services/io.github.hikaps.CouchPlayHelper.service" || true
    rm -f "$prefix/share/pipewire/pipewire-pulse.conf.d/50-couchplay.conf" || true
    rm -rf "$prefix/lib/couchplay" || true
done
rm -f /etc/dbus-1/system.d/io.github.hikaps.CouchPlayHelper.conf
rm -f /etc/systemd/system/couchplay-helper.service
rm -f /usr/share/polkit-1/actions/io.github.hikaps.couchplay.policy || true
rm -f /etc/polkit-1/actions/io.github.hikaps.couchplay.policy
for f in /etc/polkit-1/rules.d/*couchplay* /usr/share/polkit-1/rules.d/*couchplay*; do
    [[ -e "$f" ]] && rm -f "$f"
done
rm -f /etc/udev/rules.d/99-couchplay-hidraw.rules
rm -f "$DESKTOP_HOME/.config/pipewire/pipewire-pulse.conf.d/50-couchplay.conf"

if has_systemd; then
    info "Reloading systemd..."
    systemctl daemon-reload || true
fi

info "Reloading D-Bus configuration..."
reload_dbus() {
    if has_systemd && systemctl reload dbus 2>/dev/null; then
        return 0
    fi
    local pidfile dbus_pid
    for pidfile in /run/dbus/pid /var/run/dbus/pid; do
        if [[ -r "$pidfile" ]]; then
            dbus_pid="$(cat "$pidfile" 2>/dev/null || true)"
            if [[ -n "$dbus_pid" ]] && kill -HUP "$dbus_pid" 2>/dev/null; then
                warn "systemctl reload dbus unavailable; sent SIGHUP to dbus-daemon (pid ${dbus_pid})."
                return 0
            fi
        fi
    done
    return 1
}
reload_dbus || warn "Could not reload D-Bus; changes apply after a reboot."
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules 2>/dev/null || true
fi

# --- Follow-ups -----------------------------------------------------------------

echo
info "Migration finished."
echo
echo "Manual follow-ups (not done automatically, so nothing is lost by mistake):"
if [[ ${#PLAYERS[@]} -gt 0 ]]; then
    echo "  # The old helper enabled lingering, so these accounts keep a background session; stop it first:"
    echo "  sudo loginctl disable-linger ${PLAYERS[*]}"
    echo "  sudo loginctl terminate-user ${PLAYERS[*]}"
fi
for name in "${PLAYERS[@]+"${PLAYERS[@]}"}"; do
    echo "  sudo userdel -r $name"
done
echo "  # /var/lib/couchplay holds the old overlay-mode changes (overlays/<player>/); this script does not"
echo "  # import them. Delete it only after you have checked you do not need them:"
echo "  # sudo rm -rf /var/lib/couchplay"
echo
echo "The new CouchPlay needs these host packages: bubblewrap (bwrap),"
echo "dbus (dbus-run-session) and gamescope."
