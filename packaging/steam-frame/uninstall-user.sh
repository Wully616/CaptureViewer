#!/usr/bin/env bash
set -euo pipefail

root=${XDG_DATA_HOME:-${HOME:?HOME is not set}/.local/share}
[[ $root = /* && $root != *$'\n'* && $root != *$'\r'* ]] || { printf 'XDG_DATA_HOME must be an absolute single-line path\n' >&2; exit 1; }
root=$(realpath -m -- "$root")
for system_path in /usr /etc /srv; do
  [[ $root != "$system_path" && $root != "$system_path/"* ]] || { printf 'Refusing system data path: %s\n' "$root" >&2; exit 1; }
done
base=$root/captureviewer
for destination in \
  "$base" "$base/app" \
  "$root/applications" \
  "$root/icons" "$root/icons/hicolor" "$root/icons/hicolor/scalable" "$root/icons/hicolor/scalable/apps" \
  "$root/metainfo"; do
  [[ ! -L $destination ]] || { printf 'Refusing symlinked destination: %s\n' "$destination" >&2; exit 1; }
done
rm -rf -- "$base/app"
rm -f -- \
  "$root/applications/io.github.wully616.captureviewer.desktop" \
  "$root/icons/hicolor/scalable/apps/io.github.wully616.captureviewer.svg" \
  "$root/metainfo/io.github.wully616.captureviewer.metainfo.xml"
# Preserve the parent: it may contain configuration, logs, or UVC support data.
printf 'Removed the CaptureViewer user app and menu assets.\n'
