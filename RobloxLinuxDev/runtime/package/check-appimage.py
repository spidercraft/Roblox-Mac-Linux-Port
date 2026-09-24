#!/usr/bin/env python3
"""Check a built AppImage through a memfd, as used by AppImageLauncher."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

package = Path(__file__).resolve().parent
source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else package / 'roblox-mac-x86_64.AppImage'
with tempfile.TemporaryDirectory(prefix='roblox memfd ') as name:
    folder = Path(name)
    image = folder / 'RobloxLinux.AppImage'
    image.symlink_to(source)
    (folder / 'update-roblox.sh').touch()
    tmp = folder / 'DO_NOT_SHARE/tmp'
    tmp.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('ROBLOX_', 'APPIMAGE', 'APPDIR', 'TARGET_APPIMAGE'))}
    env.update(TARGET_APPIMAGE=str(image), TMPDIR=str(tmp),
               APPIMAGE_EXTRACT_AND_RUN='1', REUSE_CHECK_DELAY='0')
    # Only the runtime needs to be in memory; TARGET_APPIMAGE supplies the payload.
    fd = os.memfd_create('roblox-appimage-check', flags=0)
    try:
        with os.fdopen(os.dup(fd), 'wb') as memory:
            memory.write((package / 'tools/uruntime-x86_64').read_bytes())
        result = subprocess.run(['/proc/self/fd/' + str(fd), '--show-config'], env=env,
                                pass_fds=(fd,), capture_output=True, text=True, timeout=120)
    finally:
        os.close(fd)
    assert result.returncode == 0, result.stderr[-5000:]
    config = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
    assert config['DATA'] == str(folder / 'DO_NOT_SHARE'), config
    assert config['TMPDIR'] == str(tmp), config
print('PASS real AppImage: memfd startup, extraction without FUSE, release-local data and temp paths')
