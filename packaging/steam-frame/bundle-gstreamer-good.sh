#!/usr/bin/env bash
set -euo pipefail

fail() { printf '%s\n' "$*" >&2; exit 1; }
[[ $# == 1 ]] || fail "Usage: $0 APP_ROOT"
app=$(realpath -- "$1")
[[ -d $app && ! -L $app ]] || fail "Invalid app root: $1"
[[ $(uname -m) == aarch64 ]] || fail 'GStreamer plugin bundling requires an aarch64 builder'

for command in pacman curl bsdtar sha256sum pkg-config gst-inspect-1.0; do
  command -v "$command" >/dev/null || fail "Required build tool is missing: $command"
done

runtime_version=$(pkg-config --modversion gstreamer-1.0) || fail 'GStreamer development files are unavailable'
metadata=$(pacman -Sp --print-format '%n|%v|%l|%h' gst-plugins-good 2>/dev/null) ||
  fail 'Unable to resolve gst-plugins-good from the configured package repository'
package_version= package_url= package_sha=
while IFS='|' read -r name version url sha; do
  [[ $name == gst-plugins-good ]] || continue
  package_version=$version
  package_url=$url
  package_sha=$sha
done <<< "$metadata"
[[ -n $package_version && -n $package_url && -n $package_sha ]] ||
  fail 'Package repository did not provide gst-plugins-good download metadata'
case $package_version in
  "$runtime_version"-*) ;;
  *) fail "gst-plugins-good $package_version does not match GStreamer $runtime_version" ;;
esac
[[ $package_url == https://* && $package_sha =~ ^[[:xdigit:]]{64}$ ]] ||
  fail 'Package repository returned invalid download metadata'

work=$(mktemp -d "${TMPDIR:-/tmp}/captureviewer-gst-good.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
package=$work/gst-plugins-good.pkg.tar.zst
if ! curl --fail --location --silent --proto '=https' --proto-redir '=https' \
    --tlsv1.2 --output "$package" "$package_url" 2>"$work/curl.err"; then
  fail 'Failed to download gst-plugins-good package'
fi
read -r actual_sha _ < <(sha256sum -- "$package")
[[ $actual_sha == "$package_sha" ]] || fail 'gst-plugins-good package checksum mismatch'
read -r actual_name actual_version < <(pacman -Qp "$package")
[[ $actual_name == gst-plugins-good && $actual_version == "$package_version" ]] ||
  fail 'Downloaded package identity does not match repository metadata'

plugin_dir=$app/lib/captureviewer/gstreamer-1.0
mkdir -p -- "$plugin_dir"
for plugin in libgstvideo4linux2.so libgstjpeg.so libgstpulseaudio.so; do
  if ! bsdtar -xOf "$package" "usr/lib/gstreamer-1.0/$plugin" > "$work/$plugin" 2>/dev/null; then
    fail "gst-plugins-good package is missing $plugin"
  fi
  [[ -s $work/$plugin ]] || fail "gst-plugins-good package contains an empty $plugin"
  install -m 0755 -- "$work/$plugin" "$plugin_dir/$plugin"
done

license=/usr/share/licenses/spdx/LGPL-2.1-or-later.txt
[[ -r $license ]] || fail "Missing LGPL-2.1-or-later license text: $license"
license_dir=$app/share/licenses/captureviewer
good_version=${package_version%-*}
install -D -m 0644 -- "$license" "$license_dir/LGPL-2.1-or-later.txt"
printf 'Bundled unmodified gst-plugins-good modules, package version %s.\nLicense: LGPL-2.1-or-later; see LGPL-2.1-or-later.txt.\nSource: https://gstreamer.freedesktop.org/src/gst-plugins-good/gst-plugins-good-%s.tar.xz\n' \
  "$package_version" "$good_version" > "$license_dir/gst-plugins-good.txt"

for entry in \
  'video4linux2:libgstvideo4linux2.so:v4l2src' \
  'jpeg:libgstjpeg.so:jpegdec' \
  'pulseaudio:libgstpulseaudio.so:pulsesink'; do
  IFS=: read -r plugin module element <<< "$entry"
  if ! details=$(GST_PLUGIN_PATH_1_0="$plugin_dir" GST_REGISTRY="$work/registry.bin" \
      gst-inspect-1.0 --plugin "$plugin" 2>&1); then
    printf '%s\n' "$details" >&2
    fail "Bundled GStreamer plugin failed to load: $plugin"
  fi
  [[ $details == *"$plugin_dir/$module"* ]] ||
    fail "GStreamer did not load bundled plugin module: $module"
  if ! GST_PLUGIN_PATH_1_0="$plugin_dir" GST_REGISTRY="$work/registry.bin" \
      gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    fail "Bundled GStreamer element is unavailable: $element"
  fi
done

printf 'Bundled gst-plugins-good %s modules in %s\n' "$package_version" "$plugin_dir"
