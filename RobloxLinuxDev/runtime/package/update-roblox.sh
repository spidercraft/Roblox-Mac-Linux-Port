#!/bin/sh
# Download the current official Intel macOS client and prepare its shaders.
set -eu
umask 077
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DATA=$HERE/DO_NOT_SHARE
unset ROBLOX_MAC_DATA ROBLOX_MAC_CONFIG ROBLOX_MAC_BROWSER_DATA
mkdir -p "$DATA"
chmod 700 "$DATA"
export HOME=$DATA/home XDG_CONFIG_HOME=$DATA/config
export XDG_DATA_HOME=$DATA/share XDG_CACHE_HOME=$DATA/cache
export TMPDIR=$DATA/tmp TMP=$DATA/tmp TEMP=$DATA/tmp
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$TMPDIR"
exec 9>"$DATA/instance.lock"
flock -n 9 || { echo 'Close Roblox before updating.' >&2; exit 1; }
# Serialize updates for this release folder.
exec 8<"$HERE"
flock -n 8 || { echo 'An update is already running.' >&2; exit 1; }
version=$(sh "$HERE/run.sh" --client-version)
# Extract-and-run may print filesystem progress before the application's output.
version=$(printf '%s\n' "$version" | sed -n '/^version-[0-9a-f][0-9a-f]*$/p')
[ -n "$version" ] || { echo 'Could not read the current Roblox version.' >&2; exit 1; }
if [ "$version" = "$(cat "$HERE/RobloxVersion/.version" 2>/dev/null || true)" ] &&
   [ -x "$HERE/RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
   [ -f "$HERE/RobloxVersion/spv-cache-v1/report.json" ]; then
    echo 'Roblox is already up to date.'
    exit 0
fi
tmp=$(mktemp -d "$DATA/.update.XXXXXX")
cleanup() {
    if [ -d "$tmp/previous" ] && [ ! -e "$HERE/RobloxVersion" ]; then
        mv "$tmp/previous" "$HERE/RobloxVersion"
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
# AppImage owns the downloader and shader tools, keeping this folder small.
ROBLOX_MAC_DATA="$tmp" sh "$HERE/run.sh" --download-client
if ! ROBLOX_MAC_DATA="$tmp/runtime" ROBLOX_MAC_APP="$tmp/RobloxPlayer.app" \
    sh "$HERE/run.sh" --debug --prepare-shaders ||
   [ ! -f "$tmp/spv-cache-v1/report.json" ]; then
    echo 'Shader preparation failed; the previous client has not been changed.' >&2
    exit 1
fi
rm -rf -- "$tmp/runtime"
mkdir "$tmp/next"
mv "$tmp/RobloxPlayer.app" "$tmp/spv-cache-v1" "$tmp/.version" "$tmp/next/"
[ ! -e "$HERE/RobloxVersion" ] || mv "$HERE/RobloxVersion" "$tmp/previous"
mv "$tmp/next" "$HERE/RobloxVersion"
echo "Roblox updated: $(cat "$HERE/RobloxVersion/.version")"
