# Roblox Mac Linux Port

An experimental, unofficial runner for the Intel macOS Roblox client on x86_64 Linux, using Darling, Wayland and Vulkan.

**[Download RobloxLinuxRelease.tar.gz](https://github.com/spidercraft/Roblox-Mac-Linux-Port/releases/latest)**, extract it, run `sh update-roblox.sh` inside `RobloxLinuxRelease`, then `sh run.sh`.

```text
RobloxLinuxDev/        Source, dependency pins, patches and build scripts
RobloxLinuxRelease/    AppImage, updater, FFlags.json and README
```

The Git repository contains the development source and release instructions. The AppImage and updater are in the downloadable release asset; GitHub's automatic source-code ZIP is for developers.

## How to run

Use x86_64 Linux with glibc 2.39 or newer (Ubuntu 24.04 or newer is the build baseline), a Wayland desktop, compatible Vulkan drivers, Python 3, util-linux and unprivileged user namespaces. FUSE is optional. Other CPU architectures, X11-only sessions and musl-based systems are not supported.

The native UI and media libraries are built or packaged in Ubuntu 24.04. Every
bundled ELF is checked for baseline x86-64 and glibc 2.39 requirements; AVX-512 is
not required. The native UI is also load-tested on that baseline. The host still
supplies glibc, Wayland/XCB libraries and GPU drivers. These checks do not establish gameplay compatibility
on every distribution or GPU.

Download and extract **RobloxLinuxRelease.tar.gz** from the release link above.
Open a terminal in the extracted `RobloxLinuxRelease` folder, then run:

```sh
chmod +x RobloxLinux.AppImage
sh update-roblox.sh
sh run.sh
```

The update step downloads the official client and prepares its shaders; wait for it
to finish successfully before launching. On later launches, just run `sh run.sh`.
Keep `RobloxVersion` beside the AppImage. Use the scripts on systems with
AppImageLauncher: they prevent relocation and keep temporary paths in the release
folder, avoiding `//DO_NOT_SHARE` permission errors before Roblox starts.

`run.sh` launches with diagnostic logging and sets up the AppImage environment.
To collect system information without opening Roblox:

```sh
sh run.sh --diagnose
```

The command prints a folder under `DO_NOT_SHARE/diagnostics/` containing `system.txt`
and, for debug launches, `runtime.log`. Shader updates also save logs there.
Review individual text files before sharing: runtime output may contain account
details. Never upload the entire `DO_NOT_SHARE` folder.

Close Roblox and run `sh update-roblox.sh` from the release folder to update the official LIVE client. Updates require curl and unzip. They replace only `RobloxVersion`, preserving your login and settings. Roblox can independently expire or revoke a session. Replace the AppImage with a new release to update the compatibility runtime.

Edit `FFlags.json` to set flag overrides. It is applied on each launch; only flags supported by Roblox take effect.

## Private data

Startup creates `RobloxLinuxRelease/DO_NOT_SHARE/` for cookies, settings, browser storage, logs, caches and temporary runtime files. Native login cookies are in `DO_NOT_SHARE/cookies/native.sqlite`; browser cookies are in `DO_NOT_SHARE/browser/cookies.sqlite`.

**Never share `DO_NOT_SHARE`.** It is excluded from Git and release archives. Keep it in your installation to retain your profile. Archive tools do not obey `.gitignore`, so explicitly exclude it when sharing your own copy. The launcher redirects HOME, XDG storage and temporary paths into this folder; desktop display/audio sockets still connect to the host.

See the [release README](RobloxLinuxRelease/README.md) for usage, and the [development README](RobloxLinuxDev/README.md) to build the runtime. [Third-party notices](RobloxLinuxDev/THIRD_PARTY.md) and pinned dependency sources are included.

## Status

This is experimental. The tested LIVE client is `0.740.0.7400927` (`version-00a4ca14e31b41e9`). Startup and short TTK/Jailbreak gameplay sessions were checked; full gameplay compatibility and stable 240 FPS across games are not established. Shader preparation validated 1,938 of 1,950 shaders, with 12 unsupported translator cases remaining.

Keep the release in a reasonably short path (for example `~/RobloxLinuxRelease`). Darling can fail to start when deeply nested folder names exceed Unix-socket path limits.
