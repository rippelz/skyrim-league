#!/usr/bin/env python3
"""Cross-build with the MSVC ABI. Compilers run natively; never starts Wine or a GUI."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]

def run(args, env):
    subprocess.run([str(a) for a in args], cwd=ROOT, env=env, check=True)

def newest(directory):
    candidates = [p for p in directory.iterdir() if p.is_dir()]
    return max(candidates, key=lambda p: tuple(int(x) for x in p.name.split('.')))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--msvc-root', type=Path, default=os.environ.get('BRIDGE_MSVC_ROOT'))
    p.add_argument('--sdk-root', type=Path, help='Reuse a Windows SDK from another MSVC install')
    p.add_argument('--jobs', type=int, default=2)
    a = p.parse_args()
    if not a.msvc_root:
        p.error('Pass --msvc-root /path/to/msvc-wine-install (VC/ and Windows Kits/ required)')
    msvc = a.msvc_root.resolve()
    vc = newest(msvc/'VC/Tools/MSVC')
    sdk_root = a.sdk_root.resolve() if a.sdk_root else msvc
    sdk = newest(sdk_root/'Windows Kits/10/Include')
    includes = [vc/'include'] + [sdk/n for n in ('ucrt','shared','um','winrt','cppwinrt')]
    libs = [vc/'lib/x64', sdk_root/'Windows Kits/10/Lib'/sdk.name/'ucrt/x64', sdk_root/'Windows Kits/10/Lib'/sdk.name/'um/x64']
    aliases = ROOT/'build-win/libcase';aliases.mkdir(parents=True,exist_ok=True)
    for folder in libs:
        for library in folder.glob('*.lib'):
            for name in {library.name,library.stem.upper()+'.lib',library.name.upper()}:
                alias = aliases/name
                if alias.is_symlink():
                    alias.unlink()
                if not alias.exists():
                    alias.symlink_to(library)
    env = os.environ.copy()
    env['INCLUDE'] = ';'.join(map(str, includes))
    env['LIB'] = ';'.join(map(str, libs))
    # clang-cl on Linux reads INCLUDE, but lld-link needs explicit search paths.
    env['LDFLAGS'] = ' '.join('/libpath:"'+str(d)+'"' for d in [aliases,*libs])
    # SDK includes use Windows casing (and backslashes) on a case-sensitive host.
    # A Clang VFS overlay fixes case lookup without editing third-party sources.
    headers = ROOT/'.deps/BakkesModSDK/include'
    def directory(path):
        return {'type':'directory', 'name':path.name,
                'contents':[directory(f) if f.is_dir() else
                  {'type':'file','name':f.name,'external-contents':str(f)}
                  for f in sorted(path.iterdir())]}
    tree = directory(headers);tree['name'] = str(headers)
    overlay = ROOT/'.deps/bakkes-headers-vfs.json'
    roots=[tree]
    for header_name in ('btVector3.h','btMatrix3x3.h','../BulletCollision/CollisionShapes/btCollisionShape.h'):
        bullet_header=ROOT/'.deps/bullet-2.82/src/LinearMath'/header_name
        if not bullet_header.exists():continue
        # Bullet 2.82's Windows SIMD normalize uses an invalid shuffle lane.
        # Broadcast the reciprocal-length scalar from lane zero for clang-cl.
        patched=ROOT/'build-win'/('clang-'+Path(header_name).name)
        content=bullet_header.read_text().replace(', 0x80)',', 0)')
        if Path(header_name).name=='btCollisionShape.h':
            # RL adds getAabbSlow immediately after getAabb in its Bullet ABI.
            content=content.replace('virtual void getAabb(const btTransform& t,btVector3& aabbMin,btVector3& aabbMax) const =0;', 'virtual void getAabb(const btTransform& t,btVector3& aabbMin,btVector3& aabbMax) const =0;\n\tvirtual void getAabbSlow(const btTransform& t,btVector3& aabbMin,btVector3& aabbMax) const { getAabb(t,aabbMin,aabbMax); }')
        patched.write_text(content)
        roots.append({'type':'file','name':str(bullet_header.resolve()),'external-contents':str(patched)})
    overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':roots}))
    # Every Bullet translation unit must see the same RL-specific shape vtable.
    # A VFS file alias can miss a different include spelling and create mixed ABIs.
    # Use one private, explicitly patched source tree for the entire Windows build.
    bullet_copy=ROOT/'build-win/bullet-rl'
    from prepare_bullet import prepare
    prepare(ROOT/'.deps/bullet-2.82',bullet_copy)
    common = ['-G','Ninja' ,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
              '-DCMAKE_HAVE_LIBC_PTHREAD=1','-DCMAKE_USE_WIN32_THREADS_INIT=1',
              '-DCMAKE_SHARED_LINKER_FLAGS='+env['LDFLAGS'],'-DCMAKE_EXE_LINKER_FLAGS='+env['LDFLAGS'],
              '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'tools/msvc-toolchain.cmake')]
    prefix = ROOT/'build-win/deps'
    run(['cmake','-S','.deps/fmt','-B','build-win/fmt',*common,
         '-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DFMT_TEST=OFF','-DFMT_DOC=OFF'],env)
    run(['cmake','--build','build-win/fmt','--parallel',a.jobs],env)
    run(['cmake','--install','build-win/fmt'],env)
    run(['cmake','-S','.deps/spdlog-current','-B','build-win/spdlog-current',*common,
         '-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DCMAKE_PREFIX_PATH='+str(prefix),
         '-DSPDLOG_FMT_EXTERNAL=ON','-DSPDLOG_BUILD_TESTS=OFF','-DSPDLOG_BUILD_EXAMPLE=OFF'],env)
    run(['cmake','--build','build-win/spdlog-current','--parallel',a.jobs],env)
    run(['cmake','--install','build-win/spdlog-current'],env)
    bundle = ROOT/'.deps/commonlib-prebuilt/commonlibsse-ng-prebuilt-v7.1.0-all-msvc-cmake'
    run(['cmake','-S','.', '-B','build-win',*common,'-DBUILD_TESTING=OFF','-DBRIDGE_BUILD_PLUGINS=ON','-DBRIDGE_TEST_DYNAMIC_CONTACTS=ON',
         '-DCMAKE_PREFIX_PATH='+str(prefix),'-DBRIDGE_COMMONLIB_PREBUILT='+str(bundle),'-DBRIDGE_BULLET_ROOT='+str(bullet_copy)],env)
    run(['cmake','--build','build-win','--parallel',a.jobs],env)
    print('DLLs: build-win/bakkes-plugin/RocketSkyrim.dll and build-win/skse-plugin/SkyrimRocketBridge.dll')

if __name__ == '__main__':
    try:
        main()
    except (OSError,subprocess.CalledProcessError,ValueError) as e:
        sys.exit(str(e))
