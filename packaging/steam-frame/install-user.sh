#!/usr/bin/env bash
set -euo pipefail

usage() { printf 'Usage: %s PAYLOAD.tar.zst\n' "$0" >&2; exit 2; }
[[ $# == 1 ]] || usage
archive=$1
[[ -f $archive && -r $archive ]] || { printf 'Unreadable payload: %s\n' "$archive" >&2; exit 1; }

root=${XDG_DATA_HOME:-${HOME:?HOME is not set}/.local/share}
[[ $root = /* && $root != *$'\n'* && $root != *$'\r'* ]] || { printf 'XDG_DATA_HOME must be an absolute single-line path\n' >&2; exit 1; }
root=$(realpath -m -- "$root")
[[ $root != *%* ]] || { printf "Refusing XDG data paths containing %% because desktop launchers cannot resolve them: %s\n" "$root" >&2; exit 1; }
for system_path in /usr /etc /srv; do
  [[ $root != "$system_path" && $root != "$system_path/"* ]] || { printf 'Refusing system data path: %s\n' "$root" >&2; exit 1; }
done
base=$root/captureviewer
app=$base/app
parent=$(dirname -- "$base")
mkdir -p -- "$parent"
work=$(mktemp -d "$parent/.captureviewer-install.XXXXXXXX")
success=false
app_activated=false
backup_dir="$work/old-assets"
targets=()
installed_targets=()
new_asset_sources=()
cleanup() {
  local target
  set +e
  if [[ $success != true ]]; then
    for target in "${installed_targets[@]}"; do rm -f -- "$target"; done
    for i in "${!new_asset_sources[@]}"; do
      if [[ ! -e ${new_asset_sources[i]} ]]; then rm -f -- "${targets[i]}"; fi
    done
    for target in "${targets[@]}"; do
      if [[ -e $backup_dir$target || -L $backup_dir$target ]]; then
        rm -f -- "$target"
        mkdir -p -- "$(dirname -- "$target")"
        mv -- "$backup_dir$target" "$target"
      fi
    done
    if [[ -e $work/old-app ]]; then
      rm -rf -- "$app"
      mv -- "$work/old-app" "$app"
    elif [[ $app_activated == true || -e $work/app-swap-started ]]; then
      rm -rf -- "$app"
    fi
  fi
  rm -rf -- "$work"
}
trap cleanup EXIT

# Reject traversal and non-regular entries before extraction. Only directories and files
# are accepted; links and special files could redirect extraction outside the staging tree.
while IFS= read -r member; do
  [[ -n $member && $member != /* && $member != *$'\\'* ]] || { printf 'Unsafe archive member\n' >&2; exit 1; }
  trimmed=${member%/}
  [[ -n $trimmed && /$trimmed/ != *'/../'* && /$trimmed/ != *'/./'* ]] || { printf 'Unsafe archive member: %s\n' "$member" >&2; exit 1; }
  [[ $trimmed == app || $trimmed == app/* ]] || { printf 'Unexpected archive root: %s\n' "$member" >&2; exit 1; }
done < <(tar --zstd -tf "$archive")
while IFS= read -r listing; do
  type=${listing:0:1}
  [[ $type == - || $type == d ]] || { printf 'Unsupported archive entry type: %s\n' "$type" >&2; exit 1; }
done < <(tar --zstd -tvf "$archive")
mkdir -- "$work/extract"
tar --zstd -xf "$archive" -C "$work/extract" --no-same-owner
[[ -d $work/extract/app && ! -L $work/extract/app ]] || { printf 'Payload must contain app/\n' >&2; exit 1; }
if find "$work/extract/app" -type l -print -quit | grep -q .; then
  printf 'Symlinks are not permitted in the payload\n' >&2; exit 1
fi
[[ -f $work/extract/app/bin/captureviewer && -x $work/extract/app/bin/captureviewer ]] || {
  printf 'Payload is missing executable app/bin/captureviewer\n' >&2; exit 1;
}

# Stage alongside destination so rename stays on one filesystem.
install -d -- "$work/staged"
cp -a -- "$work/extract/app" "$work/staged/app"
icon_source="$work/staged/app/share/icons/hicolor/scalable/apps/io.github.wully616.captureviewer.svg"
meta_source="$work/staged/app/share/metainfo/io.github.wully616.captureviewer.metainfo.xml"
[[ -f $icon_source && ! -L $icon_source ]] || { printf 'Payload is missing the CaptureViewer SVG icon\n' >&2; exit 1; }
[[ -f $meta_source && ! -L $meta_source ]] || { printf 'Payload is missing CaptureViewer AppStream metadata\n' >&2; exit 1; }
icon_name=$(basename -- "$icon_source")
candidate="$work/staged/app/share/applications/io.github.wully616.captureviewer.desktop"
[[ -f $candidate && ! -L $candidate ]] || { printf 'Payload is missing the CaptureViewer desktop entry\n' >&2; exit 1; }
desktop_name=$(basename -- "$candidate")
mkdir -p -- "$work/assets"
[[ -z $icon_name ]] || install -D -- "$icon_source" "$work/assets/$icon_name"
[[ -z $meta_source ]] || install -D -- "$meta_source" "$work/assets/$(basename -- "$meta_source")"
# Escape desktop Exec arguments per Desktop Entry string rules.
escape_exec() {
  local input=$1 character escaped=
  while [[ -n $input ]]; do
    character=${input:0:1}
    input=${input:1}
    case "$character" in
      '\\'|'"'|'$') escaped+="\\$character" ;;
      *)
        if [[ $character == $'\\x60' ]]; then escaped+="\\$character"; else escaped+=$character; fi
        ;;
    esac
  done
  printf '%s' "$escaped"
}
installed_binary=$app/bin/captureviewer
escaped=$(escape_exec "$installed_binary")
new_assets="$work/new-assets"
mkdir -p -- "$new_assets/applications" "$new_assets/icons/hicolor/scalable/apps" "$new_assets/metainfo"
{
  printf '[Desktop Entry]\nType=Application\nName=CaptureViewer\nExec="%s"\n' "$escaped"
  printf 'Icon=io.github.wully616.captureviewer\nTerminal=false\nCategories=AudioVideo;\n'
} > "$new_assets/applications/$desktop_name"
cp -- "$work/assets/$icon_name" "$new_assets/icons/hicolor/scalable/apps/$icon_name"
cp -- "$work/assets/$(basename -- "$meta_source")" "$new_assets/metainfo/$(basename -- "$meta_source")"

# Refuse symlinked destination directories before creating or replacing anything.
check_not_symlink() { [[ ! -L $1 ]] || { printf 'Refusing symlinked destination: %s\n' "$1" >&2; exit 1; }; }
for destination in \
  "$base" "$app" \
  "$root/applications" \
  "$root/icons" "$root/icons/hicolor" "$root/icons/hicolor/scalable" "$root/icons/hicolor/scalable/apps" \
  "$root/metainfo"; do
  check_not_symlink "$destination"
done
install -d -- "$base"
# Keep the prior install until the new tree is ready, and restore it if activation fails.
if [[ -e $app ]]; then mv -- "$app" "$work/old-app"; fi
: > "$work/app-swap-started"
mv -- "$work/staged/app" "$app"
app_activated=true
asset_dirs=(applications icons/hicolor/scalable/apps metainfo)
for d in "${asset_dirs[@]}"; do mkdir -p -- "$root/$d"; done
desktop_target="$root/applications/io.github.wully616.captureviewer.desktop"
icon_target="$root/icons/hicolor/scalable/apps/io.github.wully616.captureviewer.svg"
meta_target="$root/metainfo/$(basename -- "$meta_source")"
backup_dir="$work/old-assets"
mkdir -p -- "$backup_dir"
targets=("$desktop_target" "$icon_target" "$meta_target")
new_asset_sources=(
  "$new_assets/applications/$desktop_name"
  "$new_assets/icons/hicolor/scalable/apps/$icon_name"
  "$new_assets/metainfo/$(basename -- "$meta_source")"
)
for target in "${targets[@]}"; do
  if [[ -e $target || -L $target ]]; then
    mkdir -p -- "$backup_dir$(dirname -- "$target")"
    mv -- "$target" "$backup_dir$target"
  fi
done
mv -- "${new_asset_sources[0]}" "$desktop_target"
installed_targets+=("$desktop_target")
mv -- "${new_asset_sources[1]}" "$icon_target"
installed_targets+=("$icon_target")
mv -- "${new_asset_sources[2]}" "$meta_target"
installed_targets+=("$meta_target")
success=true
rm -rf -- "$work/old-app" "$backup_dir"
printf 'Installed CaptureViewer to %s\n' "$app"
