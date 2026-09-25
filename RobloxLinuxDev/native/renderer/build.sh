#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$HERE/../../runtime
OUT=${OUT:-$HERE/../build/renderer}
export TMPDIR=$OUT/tmp
mkdir -p "$OUT" "$TMPDIR"
c++ -std=c++17 -O2 -I "$A/src/Vulkan-Headers-1.3.290/include" "$HERE/check.cpp" -o "$OUT/memory-type-check"
"$OUT/memory-type-check"
c++ -std=c++17 -O2 "$HERE/presentation-semaphore-check.cpp" -o "$OUT/presentation-semaphore-check"
"$OUT/presentation-semaphore-check"
python3 "$HERE/presentation-route-check.py"
python3 "$HERE/compute-layout-check.py"
python3 "$HERE/compute-sync-check.py"
python3 "$HERE/command-sync-check.py"
c++ -std=c++17 -O2 -pthread "$HERE/pipeline-workers-check.cpp" -o "$OUT/pipeline-workers-check"
"$OUT/pipeline-workers-check"
set --
for source in "$A"/src/indium/src/indium/*.cpp; do
    case "$(basename "$source")" in buffer.cpp|command-buffer.cpp|device.cpp|render-command-encoder.cpp|render-pipeline.cpp|compute-pipeline.cpp|library.cpp) ;; *) set -- "$@" "$source" ;; esac
done
set -- "$HERE/render-pipeline.cpp" "$HERE/compute-pipeline.cpp" "$HERE/library.cpp" "$@"
clang++ --target=x86_64-apple-macos11 -DPLATFORM_MacOSX -DTRACKB_RENDERER -std=c++17 -fblocks -O2 -fuse-ld=lld -Wl,-no_adhoc_codesign -dynamiclib -DDARLING \
    -isysroot "$A/darling-root" -nostdinc++ -isystem "$A/src/darling/src/external/libcxx/include" \
    -isystem "$A/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include" \
    -Wno-nullability-completeness -DTARGET_OS_WASI=0 -I "$A/src/darling/src/frameworks/CoreServices/include" -DDARLING_METAL_ENABLED=1 -I "$A/src/darling/src/external/cocotron/QuartzCore" -I "$A/src/darling/src/external/metal/private-include" \
    -F "$A/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks" -framework OpenGL -framework Foundation \
    -I "$HERE" -I "$A/src/indium/include" -I "$A/src/indium/private-include" \
    -I "$A/src/Vulkan-Headers-1.3.290/include" -L "$A/shims/metal" -liridium \
    -install_name /usr/lib/darling/libindium.dylib \
    -o "$OUT/libindium.new.dylib" "$HERE/uploads.cpp" "$HERE/swapchain.cpp" "$HERE/texture-readback.mm" "$HERE/drawable-reset.mm" "$HERE/drawable-memory.mm" "$HERE/buffer.cpp" "$HERE/command-buffer.cpp" "$HERE/device.cpp" "$HERE/render-command-encoder.cpp" "$@"
cp "$A/shims/metal/sort-weak-bindings.py" "$OUT/sort-weak-bindings.py"
python3 "$OUT/sort-weak-bindings.py" "$OUT/libindium.new.dylib"
mv "$OUT/libindium.new.dylib" "$OUT/libindium.dylib"
