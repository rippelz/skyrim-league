#!/usr/bin/env python3
"""Build in an x64 Visual Studio developer command prompt, using the same RL ABI as Linux."""
import argparse,os,shutil,subprocess
from pathlib import Path
from prepare_bullet import prepare
ROOT=Path(__file__).resolve().parents[1]
def run(command):subprocess.run([str(v) for v in command],cwd=ROOT,check=True)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--jobs',type=int,default=4);a=p.parse_args()
    if os.name!='nt':p.error('Run in an x64 Native Tools Command Prompt on Windows.')
    if not shutil.which('cl'):p.error('Open the x64 Native Tools Command Prompt for Visual Studio (C++ workload).')
    if not shutil.which('ninja') or not shutil.which('cmake'):p.error('Install CMake and Ninja (py -3 -m pip install cmake ninja).')
    run(['py','-3',ROOT/'tools/fetch_deps.py'])
    bullet=prepare(ROOT/'.deps/bullet-2.82',ROOT/'build-win/bullet-rl')
    prefix=ROOT/'build-win/deps'
    common=['-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL']
    for name,flags in [('fmt',['-DFMT_TEST=OFF','-DFMT_DOC=OFF']),('spdlog-current',['-DSPDLOG_FMT_EXTERNAL=ON','-DSPDLOG_BUILD_TESTS=OFF','-DSPDLOG_BUILD_EXAMPLE=OFF'])]:
        folder=ROOT/'build-win'/name
        run(['cmake','-S',ROOT/'.deps'/name,'-B',folder,*common,'-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DCMAKE_PREFIX_PATH='+str(prefix),*flags])
        run(['cmake','--build',folder,'--parallel',a.jobs]);run(['cmake','--install',folder])
    bundle=ROOT/'.deps/commonlib-prebuilt/commonlibsse-ng-prebuilt-v7.1.0-all-msvc-cmake'
    run(['cmake','-S',ROOT,'-B',ROOT/'build-win',*common,'-DBRIDGE_BUILD_PLUGINS=ON','-DBRIDGE_TEST_DYNAMIC_CONTACTS=ON','-DBUILD_TESTING=ON','-DBRIDGE_COMMONLIB_PREBUILT='+str(bundle),'-DBRIDGE_BULLET_ROOT='+str(bullet),'-DCMAKE_PREFIX_PATH='+str(prefix)])
    run(['cmake','--build',ROOT/'build-win','--parallel',a.jobs])
    run(['ctest','--test-dir',ROOT/'build-win','-R','bridge_(core|timing|contacts|windows_tools)$','--output-on-failure'])
if __name__=='__main__':main()
