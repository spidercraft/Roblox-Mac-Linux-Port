#!/bin/sh
# Build roblox-mac-x86_64.AppImage from runtime/. Nothing installed, nothing outside the folder.
# The Roblox client is NOT bundled (licensing, weekly updates): the AppImage downloads it on first
# run into the configured data directory. Needs: unprivileged user namespaces on the target machine.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
TOP=$(CDPATH= cd -- "$ROOT/.." && pwd)
# An ignored dependency checkout must not contain changes absent from the source release.
python3 - "$TOP" <<'CHECK_PATCHES'
import hashlib, json, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1])
server_symbols = subprocess.check_output(['nm', '-C', str(root / 'runtime/darlingserver')], text=True)
if 'DarlingServer::MessageQueue::pushOrSend(' not in server_symbols:
    raise SystemExit('Build and install the optimized immediate-reply Darling server before packaging')
receipt = root / 'runtime/optimized/darling/manifest.json'
if not receipt.is_file():
    raise SystemExit('Build and install the optimized Darling base runtime before packaging')
records = json.loads(receipt.read_text())
foundation = records.get('darling-root/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation')
if not foundation or foundation['installed'] != foundation['stock']:
    raise SystemExit('Restore stock Foundation before packaging: the optimized build breaks Roblox Home')
for name, record in records.items():
    if hashlib.sha256((root / 'runtime' / name).read_bytes()).hexdigest() != record['installed']:
        raise SystemExit(f'Optimized Darling artifact was overwritten: {name}; rebuild before packaging')
def exports(path):
    return set(subprocess.check_output(['llvm-nm', '--defined-only', '--extern-only', '-j', str(path)], text=True).splitlines())
clocks = {'_AudioGetCurrentHostTime', '_AudioGetHostClockFrequency', '_AudioGetHostClockMinimumTimeDelta',
          '_AudioConvertHostTimeToNanos', '_AudioConvertNanosToHostTime'}
if not clocks <= exports(root / 'runtime/darling-root/System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio'):
    raise SystemExit('Build and install the corrected CoreAudio clock before packaging')
if clocks & exports(root / 'runtime/shims/fill/librbxfill.dylib'):
    raise SystemExit('Regenerate fill after installing CoreAudio; its placeholders shadow the audio clock')
for name, spec in json.loads((root / 'third_party/dependencies.json').read_text()).items():
    if 'overlay' in spec:
        overlay = root / spec['overlay']
        for source in overlay.rglob('*'):
            if source.is_file() and source.read_bytes() != (root / spec['destination'] / source.relative_to(overlay)).read_bytes():
                raise SystemExit(f'{name}: source overlay differs at {source.relative_to(overlay)}; rerun bootstrap.py')
    if 'patch' not in spec:
        continue
    checkout = root / spec['destination']
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=checkout, text=True).strip() != spec['commit']:
        raise SystemExit(f'{name}: checkout does not match the pinned commit')
    actual = subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=checkout)
    if actual != (root / spec['patch']).read_bytes():
        raise SystemExit(f'{name}: export dependency changes to {spec["patch"]} before packaging')
CHECK_PATCHES
APPDIR=$HERE/AppDir
OUT=$HERE/roblox-mac-x86_64.AppImage
TOOL=$HERE/tools/appimagetool
RUNTIME=$HERE/tools/uruntime-x86_64
[ -x "$TOOL" ] || { echo "missing $TOOL: curl -L -o $TOOL https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" >&2; exit 1; }
[ -d "$ROOT/darling-root" ] || { echo "run scripts/m1-darling.sh first" >&2; exit 1; }
[ -f "$RUNTIME" ] || { echo "missing cached AppImage type2 runtime: $RUNTIME" >&2; exit 1; }
mkdir -p "$HERE/tmp"
export TMPDIR="$HERE/tmp"

rm -rf "$APPDIR"; mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/licenses/roblox-mac"
cp "$ROOT/profiler/vendor/imgui-1.91.9b/LICENSE.txt" "$APPDIR/usr/share/licenses/roblox-mac/Dear-ImGui.txt"
cp "$TOP/THIRD_PARTY.md" "$APPDIR/usr/share/licenses/roblox-mac/"
cp "$TOP/third_party/licenses/uruntime.txt" "$APPDIR/usr/share/licenses/roblox-mac/"

# Runtime pieces. darling-root is copied with cp -a (737 MB, ~10 s); hard links would break on FUSE.
cp -a "$ROOT/bin" "$ROOT/darling-root" "$ROOT/darlingserver" "$ROOT/darling-cli" "$APPDIR/"
mkdir "$APPDIR/scripts"
cp "$ROOT/scripts/fetch-client.sh" "$ROOT/scripts/runtime-limits.py" "$ROOT/scripts/launch-settings.sh" "$ROOT/scripts/diagnostics.py" "$APPDIR/scripts/"
date -u +%Y-%m-%dT%H:%M:%SZ > "$APPDIR/BUILD-ID"
git -C "$TOP" rev-parse HEAD >> "$APPDIR/BUILD-ID"
mkdir -p "$APPDIR/shims"
for s in "$ROOT"/shims/*/; do n=$(basename "$s"); mkdir -p "$APPDIR/shims/$n"; cp "$s"/*.dylib "$APPDIR/shims/$n/" 2>/dev/null || true; done
cp -a "$ROOT/shims/metal/runtime-shaders" "$APPDIR/shims/metal/"
cp "$ROOT/shims/cfnet/libcookies-host.so" "$APPDIR/shims/cfnet/"
cp "$ROOT/config.env.example" "$APPDIR/"
python3 "$HERE/check-portable.py"
if [ -f "$ROOT/optimized/ready" ]; then
    # Only runtime artifacts, never build caches or development source trees.
    python3 "$ROOT/optimized/package.py" "$APPDIR"
fi
# Shader pipeline: the metal2vulkan binary plus the two scripts fill $DATA/spv-cache on the user's machine.
mkdir -p "$APPDIR/metal"; cp "$ROOT/metal/pack2air.py" "$ROOT/metal/translate-all.sh" "$ROOT/metal/translate-one.py" "$ROOT/metal/prepare-cache.py" "$APPDIR/metal/"
cp "$TOP/metal2vulkan/target/release/metal2vulkan" "$APPDIR/usr/bin/"
python3 "$HERE/bundle-browser.py" "$APPDIR"

cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
# bin/roblox-mac resolves configuration from the original $APPIMAGE location.
exec "$APPDIR/bin/roblox-mac" "$@"
RUN
chmod +x "$APPDIR/AppRun"
cat > "$APPDIR/roblox-mac.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Roblox (macOS client)
Exec=roblox-mac %u
Icon=roblox-mac
Categories=Game;
MimeType=x-scheme-handler/roblox-player;x-scheme-handler/roblox;
Terminal=false
DESK
python3 - "$APPDIR/roblox-mac.png" <<'PY'
# 256x256 flat icon, no dependencies; replace with real art whenever.
import sys, zlib, struct
w = h = 256
raw = b''.join(b'\0' + bytes([0xE2, 0x23, 0x1A, 0xFF] * w) for _ in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
PY
ln -sf roblox-mac.png "$APPDIR/.DirIcon"
python3 "$HERE/check-portable.py" --appdir "$APPDIR"

# Keep an existing mounted AppImage's inode intact, and retain the last working
# artifact if packaging fails. The replacement is atomic on the same filesystem.
NEXT=$(mktemp "$HERE/.roblox-appimage.XXXXXX")
trap 'rm -f -- "$NEXT"' EXIT
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --runtime-file "$RUNTIME" -n "$APPDIR" "$NEXT"
chmod 755 "$NEXT"
# uruntime's default extraction fallback stops at 350 MiB; this image bundles LLVM.
sed -i 's/URUNTIME_EXTRACT=3/URUNTIME_EXTRACT=2/g' "$NEXT"
# run.sh supplies private TMPDIR; uruntime supplies content-specific cache names.
# Do not embed paths based on URUNTIME_DIR: AppImageLauncher's memfd resolves to /.
mv -f -- "$NEXT" "$OUT"
ls -lh "$OUT"
