#!/bin/bash
# Adapted from shadowcast-on-frame src/build.sh at 29da3bcb18d9d445dffae7a957a29eb0a8b60001.
# Copyright shadowcast-on-frame contributors; see LICENSE.
# Builds modules unprivileged for exactly the running SteamOS kernel.
set -euo pipefail

OUT="${1:?usage: build.sh <outdir>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
log() { echo "==> $*"; }
die() { echo "error: $*" >&2; exit 1; }
[ "$EUID" -ne 0 ] || die "build as the logged-in user, not root"
[ "$(uname -m)" = aarch64 ] || die "this module build is for Steam Frame (aarch64)"
grep -q '^ID=steamos$' /etc/os-release || die "this module build is for SteamOS"
for tool in pacman curl bsdtar make gcc python3 modinfo readelf sha256sum; do
    command -v "$tool" >/dev/null || die "missing required tool: $tool"
done

KVER="$(uname -r)"
[[ "$KVER" =~ ^[A-Za-z0-9._+-]+$ ]] || die "unexpected kernel version string"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/captureviewer-uvc-build.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
url_for_log() {
    python3 - "$1" <<'PY'
import sys
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

parts = urlsplit(sys.argv[1])
host = parts.hostname or ""
if parts.port is not None:
    host += f":{parts.port}"
path = "/".join(
    "<redacted>" if "DO_NOT_SHARE_URL" in item or
    (len(item) >= 32 and all(char in "0123456789abcdefABCDEF" for char in item))
    else item
    for item in parts.path.split("/")
)
query = urlencode([
                   (key, value)
                   for key, value in parse_qsl(parts.query, keep_blank_values=True)
                   if key == "ref"
               ])
print(urlunsplit((parts.scheme, host, path, query, "")))
PY
}
fetch() {
    local url="$1" destination="$2" safe_url status
    safe_url="$(url_for_log "$url")"
    log "Downloading $safe_url"
    if curl --proto '=https' --tlsv1.2 -fsSL --retry 3 --connect-timeout 20 "$url" -o "$destination"; then
        return 0
    else
        status=$?
        die "curl failed (exit status $status) while downloading $safe_url"
    fi
}

# Resolve the installed kernel package and require the repository's matching headers.
KPKG="$(cat "/usr/lib/modules/$KVER/pkgbase" 2>/dev/null)" ||
    die "can't identify the package for kernel $KVER"
[[ "$KPKG" =~ ^[A-Za-z0-9@._+-]+$ ]] || die "invalid kernel package name"
KPKGVER="$(pacman -Q "$KPKG" | awk '{print $2}')"
HPKG="$KPKG-headers"
HVER="$(pacman -Si "$HPKG" 2>/dev/null | awk -F': *' '/^Version/{print $2; exit}')" ||
    die "matching headers package $HPKG is unavailable"
[ -n "$HVER" ] && [ "$HVER" = "$KPKGVER" ] ||
    die "$HPKG in the repositories is $HVER, but the installed kernel package is $KPKGVER; install the pending SteamOS update and reboot"
[[ "$HVER" =~ ^[A-Za-z0-9@._+-]+$ ]] || die "invalid headers package version"

# Resolve the package through pacman, then verify the downloaded archive against
# the SHA-256 checksum in that repository's package metadata.
mkdir -m 700 "$WORK/pkgcache"
PACKAGE_METADATA="$(pacman -Sp --print-format '%n|%l|%h' "$HPKG")"
PACKAGE_URL=""
PACKAGE_SHA256=""
while IFS='|' read -r package_name package_url package_sha256; do
    if [ "$package_name" = "$HPKG" ]; then
        [ -z "$PACKAGE_URL" ] || die "pacman returned duplicate metadata for $HPKG"
        PACKAGE_URL="$package_url"
        PACKAGE_SHA256="$package_sha256"
    fi
done <<< "$PACKAGE_METADATA"
[[ "$PACKAGE_URL" =~ ^https://[^[:space:]]+\.pkg\.tar\.[A-Za-z0-9.]+$ ]] ||
    die "pacman did not return a trusted HTTPS package URL"
[[ "$PACKAGE_SHA256" =~ ^[0-9a-f]{64}$ ]] ||
    die "pacman did not return a SHA-256 checksum for $HPKG"
PACKAGE_FILE="$WORK/pkgcache/headers.pkg.tar.zst"
fetch "$PACKAGE_URL" "$PACKAGE_FILE"
printf '%s  %s\n' "$PACKAGE_SHA256" "$PACKAGE_FILE" | sha256sum --check --status ||
    die "matching kernel headers package SHA-256 does not match pacman repository metadata"
read -r DOWNLOADED_NAME DOWNLOADED_VERSION _ < <(pacman -Qp "$PACKAGE_FILE")
[ "$DOWNLOADED_NAME" = "$HPKG" ] && [ "$DOWNLOADED_VERSION" = "$HVER" ] ||
    die "downloaded headers package identity/version does not match $HPKG $HVER"
mkdir -m 700 "$WORK/headers"
bsdtar -xf "$PACKAGE_FILE" -C "$WORK/headers"
BUILD="$WORK/headers/usr/lib/modules/$KVER/build"
[ -f "$BUILD/Makefile" ] && [ -f "$BUILD/Module.symvers" ] &&
    [ -f "$BUILD/include/config/kernel.release" ] ||
    die "headers package lacks the matching kernel build context"
[ "$(cat "$BUILD/include/config/kernel.release")" = "$KVER" ] ||
    die "headers package kernel.release does not match uname -r"

# Kernel.org's upstream release tag is used as a compatibility source, not
# represented as Valve's exact patched source. Verify every downloaded source
# file against the GitHub API's Git-blob SHA for that exact release tag.
BASE="${KVER%%-*}"
[[ "$BASE" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || die "cannot derive an upstream Linux release tag"
TAG="v${BASE%.0}"
RAW="https://raw.githubusercontent.com/torvalds/linux/$TAG"
API="https://api.github.com/repos/torvalds/linux/contents"
SRC="$WORK/src"
mkdir -m 700 "$SRC"
verify_blob() {
    python3 - "$1" "$2" <<'PY'
import hashlib, pathlib, sys
content = pathlib.Path(sys.argv[1]).read_bytes()
blob = b"blob " + str(len(content)).encode("ascii") + b"\0" + content
print(hashlib.sha1(blob).hexdigest())
PY
}
fetch_verified_file() {
    local relative="$1" destination="$2" metadata="$WORK/source-item.json"
    fetch "$API/$relative?ref=$TAG" "$metadata"
    local url expected
    read -r url expected < <(python3 - "$metadata" <<'PY'
import json, sys
item = json.load(open(sys.argv[1], encoding="utf-8"))
print(item.get("download_url", ""), item.get("sha", ""))
PY
)
    case "$url" in "$RAW/"*) ;; *) die "unexpected upstream source URL for $relative" ;; esac
    [[ "$expected" =~ ^[0-9a-f]{40}$ ]] || die "missing upstream source blob hash for $relative"
    fetch "$url" "$destination"
    [ "$(verify_blob "$destination" "$expected")" = "$expected" ] ||
        die "upstream source hash mismatch for $relative"
}
fetch "$API/drivers/media/usb/uvc?ref=$TAG" "$WORK/uvc.json"
python3 - "$WORK/uvc.json" > "$WORK/uvc.files" <<'PY'
import json, sys
for item in json.load(open(sys.argv[1], encoding="utf-8")):
    if item["name"].endswith((".c", ".h")):
        print(item["name"], item["sha"])
PY
[ -s "$WORK/uvc.files" ] || die "upstream UVC source listing is empty"
while read -r file blob; do
    [[ "$file" =~ ^[A-Za-z0-9_.-]+\.(c|h)$ ]] || die "unexpected upstream UVC filename"
    [[ "$blob" =~ ^[0-9a-f]{40}$ ]] || die "invalid upstream UVC source hash"
    fetch "$RAW/drivers/media/usb/uvc/$file" "$SRC/$file"
    [ "$(verify_blob "$SRC/$file" "$blob")" = "$blob" ] ||
        die "upstream source hash mismatch for $file"
done < "$WORK/uvc.files"

# Build only helper modules whose symbols are absent from Valve's Module.symvers.
exported() { grep -qE "^0x[0-9a-f]+\s+$1\s" "$BUILD/Module.symvers"; }
MODULES=()
UVC_OBJS="$(cd "$SRC" && ls uvc_*.c | sed 's/\.c$/.o/' | tr '\n' ' ')"
if ! exported uvc_format_by_guid; then
    fetch_verified_file drivers/media/common/uvc.c "$SRC/uvc.c"
    MODULES+=(uvc)
fi
if ! exported vb2_vmalloc_memops; then
    fetch_verified_file drivers/media/common/videobuf2/videobuf2-vmalloc.c "$SRC/videobuf2-vmalloc.c"
    MODULES+=(videobuf2-vmalloc)
fi
MODULES+=(uvcvideo)
{
    printf 'obj-m += %s\n' "$(printf '%s.o ' "${MODULES[@]}")"
    printf 'uvcvideo-objs := %s\n' "$UVC_OBJS"
} > "$SRC/Kbuild"

log "Building ${MODULES[*]} for $KVER from upstream Linux $TAG"
make -C "$BUILD" M="$SRC" -j"$(nproc)" CONFIG_DEBUG_INFO_BTF_MODULES= modules \
    > "$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log"; die "module build failed"; }
mkdir -p "$OUT"
for module in "${MODULES[@]}"; do
    install -m 600 "$SRC/$module.ko" "$OUT/$module.ko"
done
printf '%s\n' "${MODULES[@]}" > "$OUT/load-order"
chmod 600 "$OUT/load-order"

SOURCE_HASHES="$WORK/source-hashes"
while read -r file blob; do printf 'drivers/media/usb/uvc/%s %s\n' "$file" "$blob"; done < "$WORK/uvc.files" > "$SOURCE_HASHES"
if [ -f "$SRC/uvc.c" ]; then
    printf 'drivers/media/common/uvc.c %s\n' \
        "$(verify_blob "$SRC/uvc.c" "")" >> "$SOURCE_HASHES"
fi
if [ -f "$SRC/videobuf2-vmalloc.c" ]; then
    printf 'drivers/media/common/videobuf2/videobuf2-vmalloc.c %s\n' \
        "$(verify_blob "$SRC/videobuf2-vmalloc.c" "")" >> "$SOURCE_HASHES"
fi
LC_ALL=C sort "$SOURCE_HASHES" > "$OUT/source-files.sha256"
SOURCE_SET="$(sha256sum "$OUT/source-files.sha256" | awk '{print $1}')"
chmod 600 "$OUT/source-files.sha256"
{
    echo '[driver]'
    echo 'format=1'
    printf 'kernel-release=%s\n' "$KVER"
    printf 'source=torvalds/linux@%s\n' "$TAG"
    printf 'source-files-sha256=%s\n' "$SOURCE_SET"
    printf 'headers-package=%s\n' "$HPKG"
    printf 'headers-version=%s\n' "$HVER"
    printf 'modules=%s\n' "$(IFS=,; echo "${MODULES[*]}")"
    for module in "${MODULES[@]}"; do
        printf 'sha256-%s=%s\n' "$module" "$(sha256sum "$OUT/$module.ko" | awk '{print $1}')"
        printf 'srcversion-%s=%s\n' "$module" "$(modinfo -F srcversion "$OUT/$module.ko")"
    done
} > "$OUT/manifest.ini"
chmod 600 "$OUT/manifest.ini"
python3 "$SCRIPT_DIR/verify.py" "$OUT" "$KVER"
log "Verified modules for $KVER; header package $HPKG $HVER; source upstream Linux $TAG"
