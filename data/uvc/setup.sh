#!/bin/bash
# Builds modules as the logged-in user; pkexec runs only the fixed driver helper.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd -P)"
MODE="${1:-}"
case "$MODE" in
    install|repair|load|uninstall|status) ;;
    *) echo "usage: setup.sh install|repair|load|uninstall|status" >&2; exit 2 ;;
esac
[ "$EUID" -ne 0 ] || { echo "run this helper as your normal user" >&2; exit 1; }
command -v pkexec >/dev/null || { echo "PolicyKit pkexec is required for administrator authentication" >&2; exit 1; }

LOG_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/captureviewer"
mkdir -p -m 700 "$LOG_DIR"
LOG="$LOG_DIR/uvc-setup.log"
exec 3>&2
exec >"$LOG" 2>&1

echo "CaptureViewer UVC setup: $(date --iso-8601=seconds)"
echo "Kernel: $(uname -r)"
HELPER_DIR=/srv/captureviewer/uvc-helper
HELPER="$HELPER_DIR/captureviewer-driver-helper"
SUPPORT_DIR="$SCRIPT_DIR/../../../share/captureviewer/uvc-support"

# Native modules and unsupported hosts must never provision privileged assets.
if [ "$MODE" = install ] || [ "$MODE" = repair ]; then
    [ "$(uname -m)" = aarch64 ] || { echo "UVC module setup is only supported on Steam Frame (aarch64)" >&2; exit 1; }
    grep -q '^ID=steamos$' /etc/os-release || { echo "UVC module setup is only supported on SteamOS" >&2; exit 1; }
    if modinfo -k "$(uname -r)" uvcvideo >/dev/null 2>&1; then
        echo "Native uvcvideo is available; CaptureViewer will not replace it."
        exit 0
    fi

    # The user app and root-owned helper are separate installations. Reinstall
    # trusted assets when any expected copy is absent or stale.
    needs_bootstrap=0
    for source_and_target in \
        "$SUPPORT_DIR/driver-helper.sh:$HELPER" \
        "$SCRIPT_DIR/load.sh:$HELPER_DIR/load.sh" \
        "$SCRIPT_DIR/verify.py:$HELPER_DIR/verify.py" \
        "$SUPPORT_DIR/captureviewer-uvc.service:$HELPER_DIR/captureviewer-uvc.service"; do
        source=${source_and_target%%:*}
        target=${source_and_target#*:}
        if [ ! -f "$source" ] || [ -L "$source" ] ||
            [ ! -f "$target" ] || [ -L "$target" ] || ! cmp -s -- "$source" "$target"; then
            needs_bootstrap=1
        fi
    done
    if [ "$needs_bootstrap" -eq 1 ]; then
        BOOTSTRAP="$SCRIPT_DIR/bootstrap-support.sh"
        if [ ! -f "$BOOTSTRAP" ] || [ ! -x "$BOOTSTRAP" ] || [ -L "$BOOTSTRAP" ]; then
            echo "The CaptureViewer UVC support bootstrap script is missing or unsafe: $BOOTSTRAP" >&3
            exit 1
        fi
        CAPTUREVIEWER_APP_DIR="$APP_ROOT" "$BOOTSTRAP"
    fi
fi

if [ ! -f "$HELPER" ] || [ ! -x "$HELPER" ] || [ -L "$HELPER" ] ||
    [ "$(stat -c '%u' "$HELPER" 2>/dev/null || true)" != 0 ]; then
    echo "The root-owned CaptureViewer UVC support helper is missing or unsafe; install support through the trusted system setup." >&3
    exit 1
fi
mode="$(stat -c '%a' "$HELPER")"
if (( (8#$mode & 0022) != 0 )); then
    echo "The root-owned CaptureViewer UVC support helper is writable by non-root users; refusing privileged operation." >&3
    exit 1
fi
for path in /srv /srv/captureviewer "$HELPER_DIR"; do
    if [ ! -d "$path" ] || [ -L "$path" ] ||
        [ "$(stat -c '%u' "$path" 2>/dev/null || true)" != 0 ]; then
        echo "The root-owned CaptureViewer UVC support directory is missing or unsafe: $path" >&3
        exit 1
    fi
    mode="$(stat -c '%a' "$path")"
    if (( (8#$mode & 0022) != 0 )); then
        echo "The root-owned CaptureViewer UVC support directory is writable by non-root users: $path" >&3
        exit 1
    fi
done
if [ "$MODE" = status ]; then
    exec "$HELPER" status
fi
if [ "$MODE" = load ] || [ "$MODE" = uninstall ]; then
    exec pkexec "$HELPER" "$MODE"
fi
# The user app and root-owned helper are separate installations. Refuse to
# build modules with one revision and validate them with another.
for source_and_target in \
    "$SUPPORT_DIR/driver-helper.sh:$HELPER" \
    "$SCRIPT_DIR/load.sh:$HELPER_DIR/load.sh" \
    "$SCRIPT_DIR/verify.py:$HELPER_DIR/verify.py" \
    "$SUPPORT_DIR/captureviewer-uvc.service:$HELPER_DIR/captureviewer-uvc.service"; do
    source=${source_and_target%%:*}
    target=${source_and_target#*:}
    if [ ! -f "$source" ] || [ -L "$source" ] ||
        [ ! -f "$target" ] || [ -L "$target" ] || ! cmp -s -- "$source" "$target"; then
        echo "error: trusted UVC helper files are missing or outdated; review and rerun bootstrap-support.sh before installing support (mismatch: $(basename "$target"))"
        exit 1
    fi
done

umask 077
WORK="$(mktemp -d /tmp/captureviewer-uvc.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -m 700 "$WORK/out"
"$SCRIPT_DIR/build.sh" "$WORK/out"
pkexec "$HELPER" "$MODE" "$WORK/out"
