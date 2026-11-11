#!/usr/bin/env python3
"""Build the pinned real drm2kgsl backend without unrelated GL/Venus renderers."""
import concurrent.futures
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
SRC = Path(os.environ['VIRGLRENDERER_SOURCE']).resolve()
PM4 = Path(os.environ['ADRENO_PM4_SOURCE']).resolve()
OUT = HERE / 'build/native-context'
NDK = Path(os.environ['ANDROID_HOME']) / 'ndk/28.2.13676358'
CC = NDK / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
SOURCES = [
    'src/drm/drm2kgsl/drm2kgsl_renderer.c', 'src/drm/drm_context.c',
    'src/drm/drm_fence.c', 'src/drm/drm_util.c', 'src/virgl_fence.c',
    'src/virgl_util.c', 'src/virgl_resource.c', 'src/virgl_context.c',
    'src/gallium/auxiliary/util/u_hash_table.c',
    'src/mesa/util/hash_table.c', 'src/mesa/util/ralloc.c',
    'src/mesa/util/os_file.c', 'src/mesa/util/anon_file.c',
    'src/mesa/util/os_misc.c', 'src/mesa/util/u_debug.c',
]
INCLUDES = ['src', 'src/drm', 'src/drm/drm-uapi', 'src/gallium/auxiliary',
            'src/gallium/auxiliary/util', 'src/gallium/include', 'src/mesa',
            'src/mesa/compat', 'src/mesa/pipe', 'src/mesa/util']

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    global OUT
    parser = argparse.ArgumentParser()
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--transport', action='store_true')
    modes.add_argument('--service', action='store_true')
    args = parser.parse_args()
    if args.service:
        OUT = HERE / 'build/service'
    if args.transport:
        OUT = HERE / 'build/transport'
    OUT.mkdir(parents=True, exist_ok=True)
    config = (SRC / 'prebuilt-intermediates/config.h').read_text()
    for macro in ('HAVE_EPOXY_EGL_H', 'ENABLE_VENUS', 'ENABLE_VULKAN_DLOAD',
                  'ENABLE_RENDER_SERVER_WORKER_THREAD'):
        old = '#define ' + macro + ' 1'
        pattern = '^' + re.escape(old) + '$'
        if len(re.findall(pattern, config, flags=re.M)) != 1:
            raise ValueError('Unexpected upstream config: ' + macro)
        config = re.sub(pattern, '/* Standalone native context: ' + macro + ' disabled. */', config, flags=re.M)
    config += '\n#define HAVE_MEMFD_CREATE 1\n'
    (OUT / 'config.h').write_text(config)
    common = [str(CC), '--target=aarch64-linux-android30', '-std=gnu17', '-O2',
              '-g', '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
              '-D_GNU_SOURCE', '-DHAVE_CONFIG_H', '-include', str(OUT / 'config.h'),
              '-I' + str(OUT), '-I' + str(HERE / 'android-compat'),
              '-Wall', '-Wextra', '-Wno-unused-parameter']
    common += ['-I' + str(SRC / inc) for inc in INCLUDES]
    common += ['-I' + str(PM4)]
    extras = ([HERE / name for name in ('vgpu-queue.c', 'vgpu-native.c', 'vgpu-backend.c' if args.service else 'transport-smoke.c')]
              if args.transport or args.service else [HERE / 'native-context-smoke.c', PM4 / 'AdrenoPM4.c'])
    paths = [SRC / name for name in SOURCES] + extras
    if args.transport:
        paths.append(PM4 / 'AdrenoPM4.c')
    def compile_one(path):
        obj = OUT / (path.stem + '.o')
        command = common + ['-c', str(path), '-o', str(obj)]
        result = subprocess.run(command, capture_output=True, text=True)
        return path, obj, command, result
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(compile_one, paths))
    log = '\n'.join(str(p) + '\n' + r.stdout + r.stderr for p, _, _, r in results)
    (OUT / 'compile.log').write_text(log)
    failed = [p.name for p, _, _, r in results if r.returncode]
    if failed:
        print('Compilation failed:', ', '.join(failed))
        print('\n'.join(line for line in log.splitlines() if 'error:' in line)[:7000])
        raise SystemExit(1)
    exe = OUT / ('gpu-backend' if args.service else 'transport-smoke' if args.transport else 'native-context-smoke')
    link = [str(CC), '--target=aarch64-linux-android30', '-Wl,--gc-sections',
            *[str(obj) for _, obj, _, _ in results], '-llog', '-lm', '-o', str(exe)]
    result = subprocess.run(link, capture_output=True, text=True)
    (OUT / 'link.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stderr[:7000])
        raise SystemExit(result.returncode)
    inputs = {os.path.relpath(p, HERE): sha(p) for p in paths}
    inputs[os.path.relpath(PM4 / 'AdrenoPM4.h', HERE)] = sha(PM4 / 'AdrenoPM4.h')
    inputs['build.py'] = sha(Path(__file__))
    if args.transport or args.service:
        inputs.update({name: sha(HERE / name) for name in ('vgpu-queue.h', 'vgpu-native.h', 'vgpu-ipc.h', 'vgpu-host.h', 'vgpu-shared.h', 'vgpu-shared-memory.h')})
    inputs.update({str(p.relative_to(HERE)): sha(p) for p in (HERE / 'android-compat').rglob('*.h')})
    # Hash all vendored headers/config too, not just translation units.
    headers = {str(p.relative_to(SRC)): sha(p) for p in SRC.rglob('*.h')}
    receipt = {'component': 'virglrenderer drm2kgsl',
               'revision': '899a4e179a5793c5aa86bbf0799b0683c6746422',
               'scope': 'Android native-context GPU qualification',
               'inputs': inputs, 'headers': headers, 'config_sha256': sha(OUT / 'config.h'),
               'commands': [cmd for _, _, cmd, _ in results] + [link],
               'executable_sha256': sha(exe)}
    (OUT / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Built:', exe)

if __name__ == '__main__':
    main()
