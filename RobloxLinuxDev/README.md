# Build Roblox Mac Linux Port

This folder contains the runtime, native renderer, browser, shims and packaging source. Old track names, dated captures, duplicate source snapshots and investigation scripts have been removed. Small build/ABI checks remain because they catch incompatible Mach-O libraries and unsafe updater behavior.

- `runtime/`: launcher, compatibility shims, browser, profiler, shader preparation and packaging.
- `native/`: x86_64 runtime libraries and Vulkan renderer.
- `third_party/`: exact upstream revisions, archive checksums, source overlays and patches.
- `bootstrap.py`: fetches dependencies into this folder, preserving existing dependency trees.
- `build.sh`: builds the runtime and AppImage from those sources.

## Build host

The packaging recipe currently targets Arch/CachyOS x86_64. Install Python 3.11+, Git, curl, unzip, libarchive (`bsdtar`), binutils, patch, Clang/LLVM (including `ld64.lld`), GCC/C++, CMake, Ninja, Rust/Cargo, pkg-config, glslang, SPIR-V tools, Wayland protocols, GTK3, WebKitGTK 4.1, json-glib, SDL3, EGL, Xcursor, X11, OpenGL and PulseAudio development files. Darling's CMake configuration additionally checks its upstream development dependencies and names missing packages.

Packaging requires Docker on the build machine. It builds the native GTK/WebKit,
SDL and video adapters in a pinned Ubuntu 24.04 container; users do not need Docker.
Ubuntu supplies maintained UI/media dependencies, while the newer LLVM shader tools
and C++ support libraries come from SHA-256-pinned standard x86-64 Arch archives in
`runtime/package/shader-packages.lock`. Host libraries are never copied into the
release. Source builds may use the host toolchain; final packaging rejects any
remaining ELF that requires newer than glibc 2.39 or a higher CPU baseline.

The container recipe is `runtime/package/baseline.Dockerfile`. Refresh its Ubuntu
package layer with `docker build --no-cache -t roblox-baseline:24.04 - < runtime/package/baseline.Dockerfile`
to pick up security updates, then rebuild and run the compatibility checks.
The host still supplies glibc, Wayland/XCB libraries and GPU drivers. Supported targets require x86_64,
glibc 2.39+, Wayland and unprivileged user namespaces; this is not a claim of
full gameplay validation across all distributions.

The source build was checked with Clang 22.1.8, CMake 4.4.3 and Rust 1.98.1. ABI and pinned-kernel checks fail closed if a different toolchain changes required binary properties.

Allow several GB of disk space. Do not run the build as root. It downloads public source archives and the official Roblox client; it does not copy your login from the release.

```sh
cd RobloxLinuxDev
sh build.sh
```

Output: `runtime/package/roblox-mac-x86_64.AppImage`.

After building, run the source checkout with `runtime/bin/roblox-mac`; its default client and private profile are in `.build/client` and `.build/profile`.

`bootstrap.py` checks downloads against `third_party/dependencies.json`, checks out pinned Git commits and applies the included patches. Downloaded dependencies, compiler outputs, build profiles and caches are ignored by Git. Existing dependencies are preserved and the tracked source overlays reapplied; edit the overlay for changes to those files. Remove a dependency directory explicitly if you want to fetch its pinned original again.

## Make a clean release

After building, use a new output directory:

```sh
sh runtime/package/build-release.sh /path/to/new/RobloxLinuxRelease
```

The builder refuses to overwrite an existing installation. It packages the AppImage, updater, `run.sh` diagnostic launcher, instructions and empty `FFlags.json`. Users run `sh update-roblox.sh` from the extracted folder to download the client and prepare its shaders before launching. To replace your own runtime while keeping your login, close Roblox and replace only the AppImage in your existing release folder.

```sh
tar --exclude='RobloxLinuxRelease/DO_NOT_SHARE' -czf RobloxLinuxRelease.tar.gz -C /path/to/new RobloxLinuxRelease
```

## Checks and private files

`python3 runtime/package/check-release.py` runs the offline storage, flags, updater preservation/rollback and download validation checks. The full build also runs compiler/ABI checks and an isolated guest timezone check. It does not establish full gameplay compatibility.

`python3 runtime/package/check-appimage.py /path/to/RobloxLinux.AppImage` checks a built image through a memory-backed executable, reproducing AppImageLauncher's path handling and verifying extraction without FUSE and private data paths.

`python3 runtime/package/check-diagnostics.py` checks signal reporting, private log storage,
exit-status and argument forwarding, and shader log preservation. The launcher accepts
`--diagnose` for a system report or `--debug` before its usual arguments to log a run.

`python3 runtime/package/check-portable.py` checks that optimized or unverified
packages are rejected. To exercise LLVM disassembly and native UI loading on a
CPU without AVX, run it inside the baseline container with `--appdir /out`,
`--sysroot /`, `--llvm-as /portable/usr/bin/llvm-as`, and `--qemu` pointing to a
mounted `qemu-x86_64-static`. This also tests with Ubuntu's graphics loader libraries
instead of accidentally testing the build host's CPU-specific drivers.
The packaged `browser/BUILD-PACKAGES.txt` records Ubuntu/source package versions;
`browser/SHADER-PACKAGES.json` records shader package versions and SHA-256 hashes.
`--appdir` also rejects newer glibc requirements in every ELF. Packaging checks
native UI library loading and shader-tool startup inside Ubuntu 24.04 itself.

Run `python3 runtime/package/check-egl.py runtime/package/AppDir --surfaceless --vulkan`
on the baseline and in a Fedora container with Mesa EGL/Vulkan drivers,
Wayland/XCB libraries and `vulkaninfo` installed. This loads each host EGL driver
after the bundled UI, initializes Mesa EGL and creates a Vulkan instance;
it catches library conflicts that loading the UI alone misses. Wayland and XCB libraries
must stay on the host with its graphics drivers, which may need newer display symbols.

The renderer build checks compute synchronization using its production barrier code. Run `python3 native/renderer/compute-sync-check.py --gpu` to also verify dependent instance updates and buffer copies on a Vulkan GPU; this requires `glslangValidator` and the Vulkan loader development library. The optional `--negative-control` reports corruption when the dependency is omitted, without requiring undefined behavior to reproduce on every driver.

Build CoreAudio before regenerating `runtime/shims/fill`: the flat-namespace loader otherwise lets a generated placeholder override the real audio clock. Packaging rejects audio-clock placeholders and missing clock exports.

After `sh runtime/optimized/audio/build.sh`, run `python3 runtime/optimized/audio/check-capture.py` to check microphone initialization, changing PCM samples and restarts through `AudioUnitRender`. It uses a private PipeWire server and a synthetic monitor source, with no physical microphone or speaker; PipeWire, pipewire-pulse and WirePlumber are required.

The build also compiles Darling's base libraries and server at `-O2`, preserving the project's framework replacements. The server's immediate-reply implementation avoids an extra main-thread handoff on synchronous guest calls. Its socket check covers reply ordering, callbacks, a full socket and a disconnected peer; packaging rejects overwritten optimized artifacts, the stock server and unapplied source overlays. Foundation stays on the stock build: replacing only that framework fixes the optimized build's reproducible Home-loading regression (HTTP 500 from the recommendations request). The installer retains it and packaging verifies its stock checksum. A clean base-runtime build can take substantially longer than rebuilding the adapters.

The launcher tears down its guest namespace if the namespace supervisor dies. The Darling CLI also watches the server with a Linux pidfd, so a server failure cannot leave it waiting forever for the guest exit status. The base-runtime build checks normal command exit and server failure before/during that wait.

For one-window frame captures, use `--frame-trace /absolute/path/to/new.csv` before `--place-id ID`. Analyze GL/Vulkan transitions together with `python3 native/frame-times.py /path/to/new.csv --all-contexts --start 30 --duration 30`. This measures completed presentation calls, not GPU execution or compositor scanout. Keep captures under `DO_NOT_SHARE`, leave verbose renderer logging off, and run no builds during gameplay measurements.

F8 opens the normal build's Custom Settings overlay, an ImGui panel hosted by GTK
above the game. The game keeps its Vulkan presenter while the HUD is visible; skipped Cocoa
flushes do not count as frames. GTK refreshes the HUD at 15 Hz by default, independently
of game rendering. Guest sampled addresses use mapped module names and offsets in
this host-side HUD because Darwin symbol resolution requires a guest thread.

Metal application completion handlers run outside GPU polling, so a blocked handler cannot stop unrelated uploads from completing. Handlers stay ordered within each command; `waitUntilCompleted` also waits for those handlers, as required by [Metal's API](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted()). The renderer's existing check covers blocked callbacks and independent completion progress.

The release creates `DO_NOT_SHARE` beside its AppImage. Source builds use `.build/profile`, `.build/client` and runtime build/check folders; they are development outputs and must not be distributed. Publish tracked source files or the clean release archive, not a ZIP of this entire development working directory.

See [THIRD_PARTY.md](THIRD_PARTY.md) for source provenance and licenses. Internal library names such as `libtracka-*.dylib` remain for loader compatibility; there are no separate Track A/B products.
