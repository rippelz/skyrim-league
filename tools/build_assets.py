#!/usr/bin/env python3
"""Build the private-asset converter with pinned Nifly sources on Linux."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
SOURCES={
    'nifly':('https://github.com/ousnius/nifly.git','134124c2b6e6e369897abb3dc62e16e74629014e'),
    'UEViewer':('https://github.com/gildor2/UEViewer.git','a0bfb468d42be831b126632fd8a0ae6b3614f981'),
    'RLUPKTools':('https://github.com/CrunchyRL/RLUPKTools.git','8b5dac01b70503f13594a2b53b6240cd696101cd'),
}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--jobs',type=int,default=2);p.add_argument('--exporter',action='store_true');a=p.parse_args()
    for name,(url,commit) in SOURCES.items():
        folder=ROOT/'.deps'/name
        if not folder.exists():
            subprocess.run(['git','clone','--filter=blob:none','--no-checkout',url,str(folder)],check=True)
            subprocess.run(['git','-C',str(folder),'checkout',commit],check=True)
        actual=subprocess.check_output(['git','-C',str(folder),'rev-parse','HEAD'],text=True).strip()
        if actual!=commit:raise SystemExit(f'Preserve modified dependency {folder}; expected {commit}')
    header=ROOT/'.deps/json/nlohmann/json.hpp';header.parent.mkdir(parents=True,exist_ok=True)
    if not header.exists():
        header.write_bytes(urllib.request.urlopen('https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp',timeout=30).read())
    if hashlib.sha256(header.read_bytes()).hexdigest()!='aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63':raise SystemExit('Unexpected nlohmann JSON header checksum')
    subprocess.run(['cmake','-S',str(ROOT/'.deps/nifly'),'-B',str(ROOT/'build-assets/nifly'),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF'],check=True)
    subprocess.run(['cmake','--build',str(ROOT/'build-assets/nifly'),'--parallel',str(a.jobs)],check=True)
    output=ROOT/'build/assets/build_nif';output.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(['c++','-std=c++20','-O2',str(ROOT/'tools/build_nif.cpp'),'-I'+str(ROOT/'.deps/nifly/include'),'-I'+str(ROOT/'.deps/nifly/external'),'-I'+str(ROOT/'.deps/json'),str(ROOT/'build-assets/nifly/src/libnifly.a'),'-o',str(output)],check=True)
    if a.exporter:subprocess.run(['bash','build.sh'],cwd=ROOT/'.deps/UEViewer',check=True)
    print('Asset converter:',output)

if __name__=='__main__':main()
