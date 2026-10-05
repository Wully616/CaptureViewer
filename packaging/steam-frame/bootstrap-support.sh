#!/usr/bin/env bash
set -euo pipefail

fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
[[ $EUID -ne 0 ]] || fail 'run this as your normal user; pkexec is used only for /usr/bin/install'
[[ -x /usr/bin/pkexec ]] || fail 'pkexec is required for administrator authorization'
[[ -x /usr/bin/install ]] || fail '/usr/bin/install is required'

app_root=${CAPTUREVIEWER_APP_DIR:-${XDG_DATA_HOME:-${HOME:?HOME is not set}/.local/share}/captureviewer/app}
[[ $app_root = /* ]] || fail 'CaptureViewer app path must be absolute'
[[ ! -L $app_root ]] || fail 'refusing a symlinked app directory'
app_root=$(realpath -e -- "$app_root") || fail 'CaptureViewer user app is not installed'
case "$app_root" in
  /usr|/usr/*|/etc|/etc/*|/srv|/srv/*) fail "refusing system app path: $app_root" ;;
esac

support_dir="$app_root/share/captureviewer/uvc-support"
user_uvc_dir="$app_root/libexec/captureviewer/uvc"
source_files=(
  "$support_dir/driver-helper.sh"
  "$user_uvc_dir/load.sh"
  "$user_uvc_dir/verify.py"
  "$support_dir/captureviewer-uvc.service"
)
target_names=(
  captureviewer-driver-helper
  load.sh
  verify.py
  captureviewer-uvc.service
)
root_dir=/srv/captureviewer
helper_dir=$root_dir/uvc-helper

for source in "${source_files[@]}"; do
  [[ -f $source && ! -L $source && -r $source ]] || fail "missing or unsafe bootstrap source: $source"
done

check_root_directory() {
  local path=$1 mode
  [[ -d $path && ! -L $path ]] || fail "unsafe existing directory: $path"
  [[ $(stat -c '%u' -- "$path") == 0 ]] || fail "directory is not root-owned: $path"
  mode=$(stat -c '%a' -- "$path")
  (( (8#$mode & 0022) == 0 )) || fail "directory is group/world writable: $path"
}
check_root_file() {
  local path=$1 mode
  [[ -f $path && ! -L $path ]] || fail "unsafe existing helper file: $path"
  [[ $(stat -c '%u:%g' -- "$path") == 0:0 ]] || fail "helper file is not root:root: $path"
  mode=$(stat -c '%a' -- "$path")
  (( (8#$mode & 0022) == 0 )) || fail "helper file is group/world writable: $path"
}

check_root_directory /srv
for directory in "$root_dir" "$helper_dir"; do
  if [[ -e $directory || -L $directory ]]; then
    check_root_directory "$directory"
  fi
done
if [[ -d $helper_dir ]]; then
  shopt -s dotglob nullglob
  for entry in "$helper_dir"/*; do
    case "${entry##*/}" in
      captureviewer-driver-helper|load.sh|verify.py|captureviewer-uvc.service) ;;
      *) fail "refusing unrecognized helper asset: $entry" ;;
    esac
    check_root_file "$entry"
  done
fi

umask 077
stage_dir=$(mktemp -d -- "${TMPDIR:-/tmp}/captureviewer-uvc-bootstrap.XXXXXXXX") || fail 'could not create a private staging directory'
cleanup() { rm -rf -- "$stage_dir"; }
trap cleanup EXIT

for index in "${!source_files[@]}"; do
  source=${source_files[$index]}
  staged="$stage_dir/${target_names[$index]}"
  cp -- "$source" "$staged" || fail "could not stage bootstrap source: $source"
  [[ -f $staged && ! -L $staged && -r $staged ]] || fail "unsafe staged bootstrap file: $staged"
  cmp -s -- "$source" "$staged" || fail "bootstrap source changed while staging: $source"
done

printf 'Provisioning the trusted CaptureViewer helper under %s.\n' "$helper_dir"
printf 'This installs helper assets only; it does not build, install, or load modules.\n'
/usr/bin/pkexec /usr/bin/install -D -o root -g root -m 0755 -t "$helper_dir" -- \
  "$stage_dir/captureviewer-driver-helper" \
  "$stage_dir/load.sh" \
  "$stage_dir/verify.py" \
  "$stage_dir/captureviewer-uvc.service"

check_root_directory /srv
check_root_directory "$root_dir"
check_root_directory "$helper_dir"
for index in "${!target_names[@]}"; do
  staged="$stage_dir/${target_names[$index]}"
  target="$helper_dir/${target_names[$index]}"
  check_root_file "$target"
  cmp -s -- "$staged" "$target" || fail "installed helper differs from staged source: $target"
done
printf 'Trusted helper files installed under %s. No modules were installed or loaded.\n' "$helper_dir"
