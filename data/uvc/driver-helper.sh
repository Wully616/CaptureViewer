#!/bin/bash
# Fixed, narrowly scoped privileged interface for CaptureViewer UVC modules.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

ROOT=/srv/captureviewer/uvc
HELPER_ROOT=/srv/captureviewer/uvc-helper
MARKER=.captureviewer-owned
UNIT=captureviewer-uvc.service
SCRIPT_DIR="$HELPER_ROOT"
KVER="$(uname -r)"
die() { echo "error: $*" >&2; exit 1; }

is_steamos_arm64() {
    [ "$(uname -m)" = aarch64 ] && grep -q '^ID=steamos$' /etc/os-release
}
require_admin() {
    [ "$EUID" -eq 0 ] || die "administrator authorization is required"
    [[ "${PKEXEC_UID:-}" =~ ^[0-9]+$ ]] && [ "$PKEXEC_UID" -ne 0 ] ||
        die "run this helper through the system PolicyKit prompt"
    is_steamos_arm64 || die "unsupported platform; expected SteamOS on aarch64"
    [[ "$KVER" =~ ^[A-Za-z0-9._+-]+$ ]] || die "cannot safely identify the running kernel"
}

trusted_helper_files() {
    local path mode
    for file in captureviewer-driver-helper verify.py load.sh captureviewer-uvc.service; do
        path="$SCRIPT_DIR/$file"
        [ -f "$path" ] && [ ! -L "$path" ] || die "missing trusted helper file: $path"
        [ "$(stat -c '%u' "$path")" = 0 ] || die "helper is not root-owned: $path"
        mode="$(stat -c '%a' "$path")"
        (( (8#$mode & 0022) == 0 )) || die "helper is group/world writable: $path"
    done
    path="$SCRIPT_DIR"
    while :; do
        [ -d "$path" ] && [ ! -L "$path" ] || die "unsafe helper directory: $path"
        [ "$(stat -c '%u' "$path")" = 0 ] || die "helper directory is not root-owned: $path"
        mode="$(stat -c '%a' "$path")"
        (( (8#$mode & 0022) == 0 )) || die "helper directory is group/world writable: $path"
        [ "$path" = / ] && break
        path="$(dirname "$path")"
    done
}

trusted_system_directories() {
    for path in /srv /srv/captureviewer /etc /etc/systemd /etc/systemd/system; do
        [ -d "$path" ] && [ ! -L "$path" ] || die "unsafe system directory: $path"
        [ "$(stat -c '%u' "$path")" = 0 ] || die "system directory is not root-owned: $path"
        mode="$(stat -c '%a' "$path")"
        (( (8#$mode & 0022) == 0 )) || die "system directory is group/world writable: $path"
    done
}

native_uvc_available() {
    modinfo -k "$KVER" uvcvideo >/dev/null 2>&1
}

owned_root_exists() {
    local marker_mode root_mode
    [ -d "$ROOT" ] && [ ! -L "$ROOT" ] &&
        [ -f "$ROOT/$MARKER" ] && [ ! -L "$ROOT/$MARKER" ] &&
        [ "$(stat -c '%u' "$ROOT")" = 0 ] &&
        [ "$(stat -c '%u' "$ROOT/$MARKER")" = 0 ] &&
        [ "$(cat "$ROOT/$MARKER")" = "CaptureViewer UVC compatibility modules" ] ||
        return 1
    root_mode="$(stat -c '%a' "$ROOT")"
    marker_mode="$(stat -c '%a' "$ROOT/$MARKER")"
    (( (8#$root_mode & 0022) == 0 && (8#$marker_mode & 0022) == 0 ))
}

check_existing_unit() {
    local path="/etc/systemd/system/$UNIT" mode
    [ -e "$path" ] || [ -L "$path" ] || return 0
    [ -f "$path" ] && [ ! -L "$path" ] &&
        [ "$(stat -c '%u' "$path")" = 0 ] ||
        die "refusing to use or remove an unowned systemd unit"
    mode="$(stat -c '%a' "$path")"
    (( (8#$mode & 0022) == 0 )) ||
        die "systemd unit is writable by non-root users"
    cmp -s "$SCRIPT_DIR/captureviewer-uvc.service" "$path" ||
        die "systemd unit differs from the trusted CaptureViewer unit"
}

ensure_owned_root() {
    trusted_system_directories
    if [ -e "$ROOT" ]; then
        owned_root_exists || die "refusing to modify unmarked or unsafe directory $ROOT"
        mode="$(stat -c '%a' "$ROOT")"
        (( (8#$mode & 0022) == 0 )) || die "$ROOT is writable by non-root users"
    else
        install -d -o root -g root -m 0755 "$ROOT"
        printf '%s\n' 'CaptureViewer UVC compatibility modules' > "$ROOT/$MARKER"
        chown root:root "$ROOT/$MARKER"
        chmod 0644 "$ROOT/$MARKER"
    fi
}

check_staging_directory() {
    local supplied="$1" staging parent
    staging="$(realpath -e -- "$supplied")" || die "module staging directory does not exist"
    [ "$staging" = "$supplied" ] || die "staging directory must not use symlinks"
    [ -d "$staging" ] && [ ! -L "$staging" ] || die "invalid module staging directory"
    parent="$(dirname "$staging")"
    [[ "$staging" = "$parent/out" ]] || die "unexpected staging path"
    [[ "$(basename "$parent")" =~ ^captureviewer-uvc\.[[:alnum:]]{6}$ ]] ||
        die "staging directory must be a CaptureViewer private temporary directory"
    [ "$(stat -c '%u' "$parent")" = "$PKEXEC_UID" ] ||
        die "staging directory is not owned by the authenticated user"
    [ "$(stat -c '%u' "$staging")" = "$PKEXEC_UID" ] ||
        die "module output is not owned by the authenticated user"
    local mode
    mode="$(stat -c '%a' "$parent")"
    (( (8#$mode & 0077) == 0 )) || die "staging parent is accessible to other users"
    mode="$(stat -c '%a' "$staging")"
    (( (8#$mode & 0077) == 0 )) || die "staging output is accessible to other users"
    python3 "$SCRIPT_DIR/verify.py" "$staging" "$KVER"
    local pkgbase pkgversion
    pkgbase="$(cat "/usr/lib/modules/$KVER/pkgbase" 2>/dev/null)" ||
        die "cannot identify the installed kernel package"
    [[ "$pkgbase" =~ ^[A-Za-z0-9@._+-]+$ ]] || die "invalid kernel package name"
    pkgversion="$(pacman -Q "$pkgbase" | awk '{print $2}')"
    python3 - "$staging/manifest.ini" "$pkgbase-headers" "$pkgversion" <<'PY'
import configparser, sys
manifest = configparser.ConfigParser(interpolation=None, strict=True)
try:
    with open(sys.argv[1], encoding="utf-8") as stream:
        manifest.read_file(stream)
except (OSError, configparser.Error) as error:
    raise SystemExit(f"invalid module manifest: {error}")
if manifest.get("driver", "headers-package", fallback="") != sys.argv[2]:
    raise SystemExit("module bundle was built against a different kernel headers package")
if manifest.get("driver", "headers-version", fallback="") != sys.argv[3]:
    raise SystemExit("module bundle was built against a different kernel package version")
PY
}

install_bundle() {
    local action="$1" supplied="$2" staging dest module normalized expected actual mode
    require_admin
    trusted_helper_files
    trusted_system_directories
    check_existing_unit
    if native_uvc_available; then
        die "native uvcvideo is available; refusing to install or load a compatibility module"
    fi
    staging="$(realpath -e -- "$supplied")" || die "module staging directory does not exist"
    check_staging_directory "$supplied"
    while read -r module; do
        normalized="${module//-/_}"
        [ -d "/sys/module/$normalized" ] || continue
        expected="$(modinfo -F srcversion "$staging/$module.ko")"
        actual="$(cat "/sys/module/$normalized/srcversion" 2>/dev/null || true)"
        [ -n "$expected" ] && [ "$expected" = "$actual" ] ||
            die "$normalized is already loaded from a different source; no replacement was attempted"
    done < "$staging/load-order"
    if [ -e "$ROOT" ]; then
        ensure_owned_root
        validate_owned_tree
    fi
    ensure_owned_root
    dest="$ROOT/$KVER"
    [ ! -L "$dest" ] || die "refusing symlink kernel bundle: $dest"
    install -d -o root -g root -m 0755 "$dest"
    [ "$(stat -c '%u' "$dest")" = 0 ] || die "kernel bundle directory is not root-owned"
    mode="$(stat -c '%a' "$dest")"
    (( (8#$mode & 0022) == 0 )) || die "kernel bundle directory is writable by non-root users"
    rm -f "$dest/manifest.ini" "$dest/load-order" "$dest/source-files.sha256"
    for module in uvc videobuf2-vmalloc uvcvideo; do
        if grep -qx "$module" "$staging/load-order"; then
            install -o root -g root -m 0644 "$staging/$module.ko" "$dest/$module.ko"
        else
            rm -f "$dest/$module.ko"
        fi
    done
    install -o root -g root -m 0644 "$staging/load-order" "$dest/load-order"
    install -o root -g root -m 0644 "$staging/source-files.sha256" "$dest/source-files.sha256"
    install -o root -g root -m 0644 "$staging/manifest.ini" "$dest/manifest.ini"
    python3 "$SCRIPT_DIR/verify.py" "$dest" "$KVER" --require-root
    install -d -o root -g root -m 0755 "$HELPER_ROOT"
    install -o root -g root -m 0644 "$SCRIPT_DIR/captureviewer-uvc.service" \
        "/etc/systemd/system/$UNIT"
    check_existing_unit
    systemctl daemon-reload
    systemctl enable "$UNIT" >/dev/null
    systemctl restart "$UNIT"
    echo "RESULT=COMPAT_DRIVER_INSTALLED action=$action kernel=$KVER"
}

load_bundle() {
    require_admin
    trusted_helper_files
    trusted_system_directories
    check_existing_unit
    [ -f "/etc/systemd/system/$UNIT" ] || die "CaptureViewer UVC service is not installed"
    ensure_owned_root
    systemctl restart "$UNIT"
    echo "RESULT=LOAD_REQUESTED kernel=$KVER"
}


validate_owned_tree() {
    owned_root_exists || die "CaptureViewer-owned module directory is not installed"
    for entry in "$ROOT"/* "$ROOT"/.[!.]*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        name="$(basename "$entry")"
        case "$name" in
            "$MARKER")
                [ -f "$entry" ] && [ ! -L "$entry" ] &&
                    [ "$(stat -c '%u' "$entry")" = 0 ] ||
                    die "unsafe CaptureViewer-owned file: $entry"
                mode="$(stat -c '%a' "$entry")"
                (( (8#$mode & 0022) == 0 )) ||
                    die "CaptureViewer-owned file is writable by non-root users: $entry"
                ;;
            *)
                [[ "$name" =~ ^[0-9]+(\.[0-9]+){1,2}[-A-Za-z0-9._+]*$ ]] &&
                    [ -d "$entry" ] && [ ! -L "$entry" ] &&
                    [ "$(stat -c '%u' "$entry")" = 0 ] ||
                    die "unrecognized or unsafe CaptureViewer kernel directory: $name"
                mode="$(stat -c '%a' "$entry")"
                (( (8#$mode & 0022) == 0 )) ||
                    die "kernel directory is writable by non-root users: $entry"
                for child in "$entry"/* "$entry"/.[!.]*; do
                    [ -e "$child" ] || [ -L "$child" ] || continue
                    case "$(basename "$child")" in
                        manifest.ini|load-order|source-files.sha256|uvc.ko|videobuf2-vmalloc.ko|uvcvideo.ko) ;;
                        *) die "unrecognized file in CaptureViewer kernel bundle: $child" ;;
                    esac
                    [ -f "$child" ] && [ ! -L "$child" ] &&
                        [ "$(stat -c '%u' "$child")" = 0 ] ||
                        die "unsafe file in CaptureViewer kernel bundle: $child"
                    mode="$(stat -c '%a' "$child")"
                    (( (8#$mode & 0022) == 0 )) ||
                        die "kernel bundle file is writable by non-root users: $child"
                done
                ;;
        esac
    done
}

uninstall_bundle() {
    local busy=0 module normalized expected actual dir child index
    local -a modules=()
    require_admin
    trusted_helper_files
    trusted_system_directories
    check_existing_unit
    if [ -e "$ROOT" ]; then
        ensure_owned_root
        validate_owned_tree
    fi
    if [ -f "/etc/systemd/system/$UNIT" ]; then
        if systemctl is-active --quiet "$UNIT"; then
            systemctl stop "$UNIT" || die "could not stop $UNIT; leaving its files in place"
        fi
        if systemctl is-enabled --quiet "$UNIT"; then
            systemctl disable "$UNIT" >/dev/null || die "could not disable $UNIT; leaving its files in place"
        fi
        rm -f "/etc/systemd/system/$UNIT"
        rm -f "/etc/systemd/system/multi-user.target.wants/$UNIT"
        systemctl daemon-reload
    fi

    dir="$ROOT/$KVER"
    if ! native_uvc_available && [ -d "$dir" ] && [ -f "$dir/manifest.ini" ] &&
        python3 "$SCRIPT_DIR/verify.py" "$dir" "$KVER" --require-root >/dev/null 2>&1; then
        mapfile -t modules < "$dir/load-order"
        for ((index=${#modules[@]} - 1; index >= 0; index--)); do
            module="${modules[index]}"
            normalized="${module//-/_}"
            [ -d "/sys/module/$normalized" ] || continue
            expected="$(modinfo -F "srcversion" "$dir/$module.ko" 2>/dev/null || true)"
            actual="$(cat "/sys/module/$normalized/srcversion" 2>/dev/null || true)"
            if [ -n "$expected" ] && [ "$expected" = "$actual" ]; then
                if ! rmmod "$normalized" 2>/dev/null; then
                    busy=1
                    echo "Module $normalized is busy; it remains loaded until use stops or the next reboot."
                fi
            fi
        done
    fi

    if [ -e "$ROOT" ]; then
        validate_owned_tree
        for dir in "$ROOT"/*/; do
            [ -d "$dir" ] || continue
            for child in "$dir"/* "$dir"/.[!.]*; do
                [ -e "$child" ] || continue
                rm -f -- "$child"
            done
            rmdir -- "$dir"
        done
        rm -f -- "$ROOT/$MARKER"
        rmdir -- "$ROOT"
    fi
    if [ -e "$HELPER_ROOT" ]; then
        for entry in "$HELPER_ROOT"/* "$HELPER_ROOT"/.[!.]*; do
            [ -e "$entry" ] || [ -L "$entry" ] || continue
            case "$(basename "$entry")" in
                captureviewer-driver-helper|load.sh|verify.py|captureviewer-uvc.service) ;;
                *) die "unrecognized support helper asset: $entry" ;;
            esac
            [ -f "$entry" ] && [ ! -L "$entry" ] &&
                [ "$(stat -c '%u' "$entry")" = 0 ] ||
                die "unsafe support helper asset: $entry"
            mode="$(stat -c '%a' "$entry")"
            (( (8#$mode & 0022) == 0 )) ||
                die "support helper asset is writable by non-root users: $entry"
        done
        rm -f -- "$HELPER_ROOT/captureviewer-driver-helper" "$HELPER_ROOT/load.sh" \
            "$HELPER_ROOT/verify.py" "$HELPER_ROOT/captureviewer-uvc.service"
        rmdir -- "$HELPER_ROOT"
    fi
    if [ "$busy" -eq 1 ]; then
        echo "RESULT=UNINSTALLED_MODULES_STILL_LOADED"
        return 10
    fi
    echo "RESULT=UNINSTALLED"
}

status() {
    if ! is_steamos_arm64; then
        echo 'state=UNSUPPORTED_PLATFORM_OR_KERNEL'
    elif native_uvc_available; then
        echo 'state=NATIVE_UVC_AVAILABLE'
    elif [ -d /sys/module/uvcvideo ] && ! owned_root_exists; then
        echo 'state=COMPAT_DRIVER_LOADED_UNMANAGED'
    elif owned_root_exists && [ -f "$ROOT/$KVER/manifest.ini" ]; then
        trusted_helper_files
        trusted_system_directories
        if python3 "$HELPER_ROOT/verify.py" "$ROOT/$KVER" "$KVER" --require-root >/dev/null 2>&1; then
            if [ -d /sys/module/uvcvideo ]; then
                echo 'state=COMPAT_DRIVER_LOADED'
            else
                echo 'state=COMPAT_DRIVER_INSTALLED_NOT_LOADED'
            fi
        else
            echo 'state=DRIVER_UPDATE_REQUIRED'
        fi
    elif owned_root_exists; then
        echo 'state=DRIVER_UPDATE_REQUIRED'
    else
        echo 'state=DRIVER_REQUIRED'
    fi
}

case "${1:-}" in
    status)
        [ "$#" -eq 1 ] || die "usage: captureviewer-driver-helper status"
        status
        ;;
    install|repair)
        [ "$#" -eq 2 ] || die "usage: captureviewer-driver-helper install|repair STAGING_DIR"
        install_bundle "$1" "$2"
        ;;
    load)
        [ "$#" -eq 1 ] || die "usage: captureviewer-driver-helper load"
        load_bundle
        ;;
    uninstall)
        [ "$#" -eq 1 ] || die "usage: captureviewer-driver-helper uninstall"
        uninstall_bundle
        ;;
    *)
        die "usage: captureviewer-driver-helper status|install|repair|load|uninstall"
        ;;
esac
