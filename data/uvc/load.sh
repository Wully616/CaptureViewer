#!/bin/bash
# Loads only a verified CaptureViewer bundle for the exact running kernel.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

ROOT=/srv/captureviewer/uvc
KVER="$(uname -r)"
die() { echo "error: $*" >&2; exit 1; }
[ "$EUID" -eq 0 ] || die "must run as root"
[ "$(uname -m)" = aarch64 ] || die "unsupported architecture"
grep -q '^ID=steamos$' /etc/os-release || die "unsupported operating system"
[[ "$KVER" =~ ^[A-Za-z0-9._+-]+$ ]] || die "cannot identify running kernel"
[ ! -L /srv ] && [ -d /srv ] || die "unsafe /srv directory"
[ "$(stat -c '%u' /srv)" = 0 ] || die "/srv is not root-owned"
mode="$(stat -c '%a' /srv)"
(( (8#$mode & 0022) == 0 )) || die "/srv is writable by non-root users"
[ ! -L /srv/captureviewer ] && [ -d /srv/captureviewer ] ||
    die "unsafe /srv/captureviewer directory"
[ "$(stat -c '%u' /srv/captureviewer)" = 0 ] || die "/srv/captureviewer is not root-owned"
mode="$(stat -c '%a' /srv/captureviewer)"
(( (8#$mode & 0022) == 0 )) || die "/srv/captureviewer is writable by non-root users"
[ ! -L /srv/captureviewer/uvc-helper ] && [ -d /srv/captureviewer/uvc-helper ] ||
    die "unsafe CaptureViewer helper directory"
[ "$(stat -c '%u' /srv/captureviewer/uvc-helper)" = 0 ] ||
    die "CaptureViewer helper directory is not root-owned"
mode="$(stat -c '%a' /srv/captureviewer/uvc-helper)"
(( (8#$mode & 0022) == 0 )) || die "CaptureViewer helper directory is writable by non-root users"
[ ! -L "$ROOT" ] && [ -d "$ROOT" ] || die "CaptureViewer module directory is missing or unsafe"
[ "$(stat -c '%u' "$ROOT")" = 0 ] || die "CaptureViewer module directory is not root-owned"
mode="$(stat -c '%a' "$ROOT")"
(( (8#$mode & 0022) == 0 )) || die "CaptureViewer module directory is writable by non-root users"
[ -f "$ROOT/.captureviewer-owned" ] && [ ! -L "$ROOT/.captureviewer-owned" ] ||
    die "CaptureViewer ownership marker is missing"
[ "$(stat -c '%u' "$ROOT/.captureviewer-owned")" = 0 ] ||
    die "CaptureViewer ownership marker is not root-owned"
mode="$(stat -c '%a' "$ROOT/.captureviewer-owned")"
(( (8#$mode & 0022) == 0 )) || die "CaptureViewer ownership marker is writable by non-root users"
[ "$(cat "$ROOT/.captureviewer-owned")" = "CaptureViewer UVC compatibility modules" ] ||
    die "CaptureViewer ownership marker mismatch"

# Native SteamOS/Linux support always takes precedence.
if modinfo -k "$KVER" uvcvideo >/dev/null 2>&1; then
    modprobe uvcvideo || die "native uvcvideo is available but failed to load"
    echo "RESULT=NATIVE_UVC_AVAILABLE kernel=$KVER"
    exit 0
fi

DIR="$ROOT/$KVER"
[ ! -L "$DIR" ] && [ -d "$DIR" ] ||
    die "DRIVER_UPDATE_REQUIRED: no compatibility bundle for kernel $KVER; update Capture Support from the app"
[ -f "/srv/captureviewer/uvc-helper/verify.py" ] &&
    [ ! -L "/srv/captureviewer/uvc-helper/verify.py" ] || die "bundle verifier is missing"
[ "$(stat -c '%u' "/srv/captureviewer/uvc-helper/verify.py")" = 0 ] ||
    die "bundle verifier is not root-owned"
[ -f "/srv/captureviewer/uvc-helper/load.sh" ] &&
    [ ! -L "/srv/captureviewer/uvc-helper/load.sh" ] || die "loader is missing"
[ "$(stat -c '%u' "/srv/captureviewer/uvc-helper/load.sh")" = 0 ] ||
    die "loader is not root-owned"
for helper in /srv/captureviewer/uvc-helper/verify.py /srv/captureviewer/uvc-helper/load.sh; do
    mode="$(stat -c '%a' "$helper")"
    (( (8#$mode & 0022) == 0 )) || die "root loader helper is writable by non-root users: $helper"
done
python3 /srv/captureviewer/uvc-helper/verify.py "$DIR" "$KVER" --require-root
mapfile -t MODULES < "$DIR/load-order"

# Refuse conflicts before inserting any helper module.
for module in "${MODULES[@]}"; do
    normalized="${module//-/_}"
    if [ -d "/sys/module/$normalized" ]; then
        expected="$(modinfo -F srcversion "$DIR/$module.ko")"
        actual="$(cat "/sys/module/$normalized/srcversion" 2>/dev/null || true)"
        [ -n "$expected" ] && [ "$expected" = "$actual" ] ||
            die "$normalized is already loaded from a different source; no replacement was attempted"
    fi
done

for module in "${MODULES[@]}"; do
    normalized="${module//-/_}"
    if [ -d "/sys/module/$normalized" ]; then
        echo "$module already loaded from the matching bundle"
    else
        insmod "$DIR/$module.ko"
        echo "loaded $module from $DIR/$module.ko"
    fi
done
echo "RESULT=COMPAT_DRIVER_LOADED kernel=$KVER"
