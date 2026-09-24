#!/bin/sh
# Terminal launcher with diagnostics. --diagnose checks the system without starting Roblox.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# AppImageLauncher can relocate the image or run a memfd whose parent is /.
# Keep the original image and let uruntime name its cache inside our private tmp.
umask 077
mkdir -p "$HERE/DO_NOT_SHARE/tmp"
chmod 700 "$HERE/DO_NOT_SHARE"
export APPIMAGELAUNCHER_DISABLE=1 _FORCE_HEADLESS=1
export TARGET_APPIMAGE=$HERE/RobloxLinux.AppImage
unset APPIMAGE_TARGET_DIR
export TMPDIR=$HERE/DO_NOT_SHARE/tmp TMP=$HERE/DO_NOT_SHARE/tmp TEMP=$HERE/DO_NOT_SHARE/tmp
case "${1:-}" in
    --diagnose|--debug|--client-version|--download-client) exec "$HERE/RobloxLinux.AppImage" "$@";;
    *) exec "$HERE/RobloxLinux.AppImage" --debug "$@";;
esac
