#!/usr/bin/env python3
"""Build native x86_64 adapters from existing sources; never install or launch."""
from pathlib import Path
import fcntl
import hashlib
import json
import os
import re
import runpy
import shutil
import subprocess

here = Path(__file__).resolve().parent
a = here.parents[1]
b = a.parent / 'native'
out = here / 'build'
stage = out / 'stage'
stage.mkdir(parents=True, exist_ok=True)
lock = (out / 'build.lock').open('w')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
env = os.environ.copy()
env.update(TMPDIR=str(out / 'tmp'), PYTHONDONTWRITEBYTECODE='1')
Path(env['TMPDIR']).mkdir(exist_ok=True)
sdk = a / 'src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
platform = a / 'src/darling/src/external/libplatform'
flags = ['clang', '--target=x86_64-apple-macos11', '-O2', '-fno-builtin',
         '-D__DARWIN_ONLY_UNIX_CONFORMANCE=1', '-fuse-ld=lld',
         '-Wl,--threads=4,-no_adhoc_codesign', '-isysroot', str(a / 'darling-root'),
         '-nostdlibinc', '-isystem', str(sdk / 'usr/include')]
commands = []


def run(command, label):
    command = list(map(str, command))
    commands.append(command)
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=180)
    # Compiler/build diagnostics only; discard sensitive lines before storage/output.
    clean = '\n'.join(line for line in result.stdout.splitlines() if not re.search(
        r'ticket|joinscript|body:|status code:|cookie|token|authorization', line, re.I))
    (out / (label + '.log')).write_text(clean + '\n')
    if result.returncode:
        raise SystemExit(f'FAIL {label}; see filtered build/{label}.log')


def library(name, sources, extra=()):
    run(flags + list(extra) + ['-dynamiclib', '-install_name', '@rpath/' + name,
                              *sources, '-o', stage / name], name)


library('libtracka-cpu.dylib', [here / 'cpu.c', here / 'runtime-sidecar/cpu-number.c'], ['-O3'])
library('libtracka-clock.dylib', [b / 'native-clock/clock.c', here / 'runtime-sidecar/mach-clock.c'],
        ['-DTRACKA_NATIVE_CLOCK', '-DDARLING', '-DPRIVATE', '-fblocks', '-I', str(a / 'src/darling/src/external/libc/gen'),
         '-I', str(a / 'src/darling/src/startup/mldr/elfcalls')])
library('libtracka-memory.dylib', [here / 'memory.c', b / 'native-memory/fill.c', *[
    platform / 'src/string/generic' / name for name in ('memchr.c', 'memset_pattern.c')]],
    ['-O3', '-fno-strict-aliasing', '-DVARIANT_STATIC=1', '-DPRIVATE',
     '-I', str(platform / 'private'), '-I', str(platform / 'include'),
     '-I', str(platform / 'src/string/generic')])
library('librbxkqueue.dylib', [a / 'shims/kqueue' / name for name in
        ('kqueue.c', 'ulock.c', 'io.c', 'semaphore.c', 'thread-switch.c', 'workq.c', 'workq-stack.S')], ['-DPLATFORM_MacOSX'])
# This adapter overrides no libc string functions; allow their normal compiler
# optimizations in the mapping scanner (unlike the memory/clock interposers).
library('libtracka-taskinfo.dylib', [here / 'task-info.c'], ['-fbuiltin'])
run(['python3', '-B', b / 'objc-runtime/build.py', '--output-dir', out / 'objc'], 'objc-build')
shutil.copy2(out / 'objc/libobjc.A.dylib', stage / 'libobjc.A.dylib')
# Reuse the pinned source builder and ABI checker, with every output redirected.
malloc = out / 'malloc'
run(['python3', '-B', b / 'native-malloc/build.py', '--output-dir', malloc], 'malloc-build')
run(['python3', '-B', b / 'native-malloc/check-abi.py', '--output-dir', malloc], 'malloc-abi')
shutil.copy2(malloc / 'O2/libsystem_malloc.dylib', stage / 'libsystem_malloc.dylib')
shutil.copy2(malloc / 'check', out / 'malloc-check')
runtime = here / 'runtime-sidecar'
run(['python3', '-B', here / 'timezone-sidecar/build.py'], 'timezone-build')
shutil.copy2(here / 'timezone-sidecar/build/candidate/libsystem_c.dylib', stage / 'libsystem_c.dylib')
run(['python3', '-B', runtime / 'workq-semaphore/build.py'], 'workq-semaphore-build')
shutil.copy2(runtime / 'workq-semaphore/build/libsystem_kernel.dylib', stage / 'libsystem_kernel.dylib')
run(['python3', '-B', runtime / 'kqueue-delete/build-system.py'], 'kqueue-system-build')
shutil.copy2(runtime / 'kqueue-delete/build/candidate/libSystem.B.dylib', stage / 'libSystem.B.dylib')
run(['python3', '-B', runtime / 'build.py'], 'rtti-build')
run(['python3', '-B', runtime / 'abi-check.py', '--rtti-only'], 'rtti-abi')
shutil.copy2(runtime / 'build/rtti-O2/libc++abi.dylib', stage / 'libc++abi.dylib')
run(['python3', '-B', here / 'libcxx-sidecar/build.py'], 'libcxx-build')
run(['python3', '-B', here / 'libcxx-sidecar/abi-check.py'], 'libcxx-abi')
shutil.copy2(here / 'libcxx-sidecar/build/O2/libc++.1.dylib', stage / 'libc++.1.dylib')
run(['python3', '-B', here / 'metal-wrapper-sidecar/build.py'], 'metal-build')
run(['python3', '-B', here / 'metal-wrapper-sidecar/abi-check.py'], 'metal-abi')
checks = [('cpu-check', b / 'cpu-query-check.m', ['-framework', 'Foundation']),
          ('clock-check', b / 'clock-check.c', []),
          ('memory-check', here / 'check.c', []),
          ('objc-check', here / 'objc-check.m', ['-framework', 'Foundation']),
          ('task-info-check', here / 'task-info-check.c', []),
          ('host-memory-check', a / 'shims/identity/memory-check.c', []),
          *[(name + '-check', a / 'probes' / (name + '.c'), [])
            for name in ('kqueue', 'cond-wakeup', 'workq', 'semaphore', 'semaphore-destroy', 'thread-switch', 'io')]]
for name, source, extra in checks:
    run(flags + extra + [source, '-o', out / name], name)
run(['cc', '-O2', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
     b / 'cpu-topology-check.c', '-o', out / 'libcpu-check.so'], 'cpu-linux-build')
run(['python3', '-B', b / 'cpu-topology-check.py', out / 'libcpu-check.so'], 'cpu-linux-check')
# Link checks run before publishing; no consumer sees a partially written image.
macho = runpy.run_path(str(runtime / 'symbolize.py'))['macho']
for path in sorted(stage.glob('*.dylib')):
    run(['llvm-objdump', '--macho', '--dylibs-used', path], path.name + '-deps')
    dependencies = (out / (path.name + '-deps.log')).read_text()
    assert 'armrt' not in dependencies and 'translation-layer' not in dependencies
    macho(path)  # Validate x86_64 in both thin adapters and the stock fat kernel.
(here / 'lib').mkdir(exist_ok=True)
for path in sorted(stage.glob('*.dylib')):
    path.replace(here / 'lib' / path.name)
# Replace the tree: copying framework symlinks over existing ones fails.
shutil.rmtree(here / 'frameworks', ignore_errors=True)
shutil.copytree(here / 'metal-wrapper-sidecar/build/O2/frameworks', here / 'frameworks', symlinks=True)
(out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
# Small build receipt; the full pinned allocator manifest/ABI report and its
# compiler commands remain in build/malloc. Shared Objective-C recipe is hashed.
sources = [a / 'profiler/memory.h', *here.glob('*.c'), *here.glob('*.m'), here / 'build.py',
           here / 'timezone-sidecar/build.py', here / 'timezone-sidecar/localtime.patch',
           here / 'libcxx-sidecar/build.py', here / 'libcxx-sidecar/abi-check.py',
           here / 'libcxx-sidecar/linker/CMakeLists.txt',
           here / 'metal-wrapper-sidecar/build.py', here / 'metal-wrapper-sidecar/abi-check.py',
           runtime / 'build.py', runtime / 'abi-check.py', runtime / 'cpu-number.c', runtime / 'mach-clock.c', runtime / 'cpu-features.h',
           runtime / 'kqueue-delete/build.py', runtime / 'kqueue-delete/build-system.py',
           runtime / 'workq-semaphore/build.py', runtime / 'workq-semaphore/semaphore.S',
           b / 'objc-runtime/build.py', b / 'native-malloc/build.py',
           b / 'native-malloc/source-sha256.json', b / 'cpu-query.c',
           b / 'cpu-query.h', b / 'cpu-topology.h', b / 'native-clock/clock.c',
           b / 'native-memory/memory.c', b / 'native-memory/fill.c',
           a / 'src/darling/src/external/libc/gen/clock_gettime.c',
           *[a / 'shims/kqueue' / name for name in ('kqueue.c', 'ulock.c', 'io.c', 'semaphore.c', 'thread-switch.c', 'workq.c', 'workq-stack.S')],
           *[p for p in (a / 'src/darling/src/external/objc4/runtime').rglob('*')
             if p.is_file() and p.suffix in ('.h', '.mm', '.S')]]
receipt = {'target': 'x86_64-apple-macos11',
           'compiler': subprocess.check_output(['clang', '--version'], text=True).splitlines()[0],
           'sources_sha256': {str(p.relative_to(a.parent)): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(sources)},
           'libraries_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in sorted((here / 'lib').glob('*.dylib'))},
           'frameworks_sha256': {str(p.relative_to(here)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted((here / 'frameworks').glob('*.framework/Versions/A/*'))
                                 if p.is_file()}}
(out / 'provenance.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(f'PASS native build, allocator ABI, Linux CPU topology; libraries: {here / "lib"}')
