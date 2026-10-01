#!/usr/bin/env python3
"""Check installed dependencies and report actual runtime evidence; never launch games."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
from isolation import configured_proton
from install import file_version
from launch import family_sharing_refused

ROOT=Path(__file__).resolve().parents[1]

def check():
    state=json.loads((ROOT/'build/install-state.json').read_text());steam=Path(state['steam']);sky=Path(state['skyrim']);bm=Path(state['bakkesmod'])
    results=[]
    def add(name,ok,detail):results.append(dict(check=name,ok=bool(ok),detail=str(detail)))
    version='.'.join(map(str,file_version(sky/'SkyrimSE.exe')))
    add('Skyrim runtime',version==state['skyrim_version'],version)
    for name in ('skse64_loader.exe','skse64_1_7_104.dll','skse64_steam_loader.dll'):
        add(name,(sky/name).is_file(),sky/name)
    al=sky/'Data/SKSE/Plugins/versionlib-1-7-104-0.bin'
    add('Address Library',al.is_file() and struct.unpack_from('<5I',al.read_bytes())==(5,1,7,104,0),'v13 / 1.7.104.0')
    for source,target in [(ROOT/'build-win/skse-plugin/SkyrimRocketBridge.dll',sky/'Data/SKSE/Plugins/SkyrimRocketBridge.dll'),(ROOT/'build-win/bakkes-plugin/RocketSkyrim.dll',bm/'plugins/RocketSkyrim.dll')]:
        add(target.name,source.is_file() and target.is_file() and hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256(target.read_bytes()).digest(),'installed DLL matches build')
    build=re.search(r'"buildid"\s+"(\d+)"',(steam/'steamapps/appmanifest_252950.acf').read_text()).group(1)
    add('RL build',build in ('25400034','25535926'),build)
    add('BakkesMod', (bm/'version.txt').read_text().strip()=='228','228, injector 2.0.76')
    for app in (252950,489830):add(f'Proton {app}',True,configured_proton(steam,app))
    for name in ('Fennec','Ball'):add(name+' mesh',(sky/f'Data/meshes/rocketbridge/{name.lower()}.nif').is_file(),sky/f'Data/meshes/rocketbridge/{name.lower()}.nif')
    for name in ('Xvfb','xauth','bsdtar'):add(name,shutil.which(name),shutil.which(name))
    add('Offline Proton loader',(ROOT/'build-win/BridgeInjector.exe').is_file(),'verifies -NoEAC and refuses EAC-parented/multiple RL processes')
    add('Browser viewer',all(p.exists() for p in (ROOT/'.deps/viewer/usr/bin/x11vnc',ROOT/'.deps/viewer-env/bin/python',ROOT/'.deps/noVNC/vnc.html')),'local display/viewer dependencies')
    add('Private window manager',(ROOT/'.deps/viewer/usr/bin/openbox').is_file(),'Openbox on the private displays only')
    controllers=[]
    for device in Path('/sys/class/input').glob('event*/device'):
        try:
            name=(device/'name').read_text().strip()
            if re.search(r'xbox|controller|gamepad|dualshock|dualsense',name,re.I):controllers.append(name)
        except OSError:pass
    evidence={}
    for name in ('probe-rl','probe-skse'):
        report=ROOT/'build'/name/'result.json'
        evidence[name]=json.loads(report.read_text()) if report.exists() else dict(verified=False)
    live=ROOT/'build/live-validation.json'
    if live.exists():evidence['live-validation']=json.loads(live.read_text())
    console=steam/'logs/console_log.txt'
    denied=[]
    if console.exists():
        lines=console.read_text(errors='replace').splitlines()
        denied=[line for line in lines if 'AppID 489830' in line and 'FamilySharing' in line][-3:]
    return dict(installed_dependencies_ok=all(r['ok'] for r in results),checks=results,controllers=controllers,
                runtime_evidence=evidence,skyrim_family_sharing_currently_refused=family_sharing_refused(steam),
                skyrim_family_sharing_historical_errors=denied,
                controller_routing_verified=False,skyrim_terrain_collision_mirrored=False)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--json',action='store_true');p.add_argument('--output',type=Path);a=p.parse_args()
    report=check();text=json.dumps(report,indent=2)+'\n'
    if a.output:a.output.write_text(text)
    if a.json:print(text,end='')
    else:
        for r in report['checks']:print(('OK   ' if r['ok'] else 'FAIL ')+r['check']+': '+r['detail'])
        print('Controller routing: unverified. Skyrim terrain collision: experimental feedback; see live-validation evidence.')
        if report['skyrim_family_sharing_currently_refused']:print('Steam currently refuses Skyrim startup with FamilySharing; see report for timestamps.')
        print('Runtime probes:',json.dumps(report['runtime_evidence']))
    if not report['installed_dependencies_ok']:raise SystemExit(1)

if __name__=='__main__':main()
