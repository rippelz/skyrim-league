#!/usr/bin/env python3
"""Install built bridge DLLs and BakkesMod into the existing game prefixes; no GUI."""
import argparse
import hashlib
import json
import os
import tempfile
from pathlib import Path
import shutil
import struct
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SUPPORTED_RL_BUILDS = {'25400034', '25535926'}
SKYRIM_VERSION = (1, 7, 104, 0)

def file_version(path):
    raw = path.read_bytes()
    at = raw.index(struct.pack('<I', 0xFEEF04BD))
    _, _, ms, ls = struct.unpack_from('<4I', raw, at)
    return ms >> 16, ms & 65535, ls >> 16, ls & 65535

class Installer:
    def __init__(self):
        self.folder = ROOT/'build/install'/(time.strftime('%Y%m%d-%H%M%S')+'-'+str(time.time_ns()))
        self.folder.mkdir(parents=True, exist_ok=False)
        self.entries = []

    def put(self, target, content):
        target = target.resolve()
        if target.exists() and target.read_bytes() == content:
            return
        backup = None
        if target.exists():
            backup = self.folder/f'{len(self.entries):04d}.bak'
            shutil.copy2(target, backup)
        target.parent.mkdir(parents=True, exist_ok=True)
        fd,temporary=tempfile.mkstemp(prefix='.'+target.name+'-',dir=target.parent)
        try:
            with os.fdopen(fd,'wb') as out:out.write(content);out.flush();os.fsync(out.fileno())
            os.replace(temporary,target)
        finally:
            if os.path.exists(temporary):os.unlink(temporary)
        self.entries.append(dict(path=str(target), backup=str(backup) if backup else None,
                                 sha256=hashlib.sha256(content).hexdigest()))
        self.save()

    def copy(self, source, target):
        self.put(target, source.read_bytes())

    def save(self):
        (self.folder/'manifest.json').write_text(json.dumps(self.entries, indent=2)+'\n')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--steam', type=Path, default=Path.home()/'.local/share/Steam')
    p.add_argument('--address-library', type=Path, required=True, help='Downloaded Address Library v13 ZIP')
    p.add_argument('--bakkes-runtime', type=Path, default=ROOT/'.deps/bakkesmod-runtime')
    p.add_argument('--crt', type=Path, help='MSVC x64 Microsoft.VC145.CRT directory for app-local runtime DLLs')
    a = p.parse_args()
    steam = a.steam.resolve(); apps = steam/'steamapps'
    sky = apps/'common/Skyrim Special Edition'
    rl = apps/'common/rocketleague'
    import re
    build = re.search(r'"buildid"\s+"(\d+)"', (apps/'appmanifest_252950.acf').read_text()).group(1)
    if build not in SUPPORTED_RL_BUILDS:
        p.error(f'BakkesMod 228 is not verified for installed RL build {build}')
    if file_version(sky/'SkyrimSE.exe') != SKYRIM_VERSION:
        p.error('This installer requires Skyrim 1.7.104.0')
    for name in ('skse64_loader.exe', 'skse64_1_7_104.dll', 'skse64_steam_loader.dll'):
        if not (sky/name).is_file():
            p.error(f'Missing matching SKSE 2.3.1 component: {name}')
    for dll in (ROOT/'build-win/bakkes-plugin/RocketSkyrim.dll', ROOT/'build-win/skse-plugin/SkyrimRocketBridge.dll'):
        if not dll.is_file():
            p.error(f'Build first: {dll}')
    version = (a.bakkes_runtime/'files/version.txt').read_text().strip()
    if version != '228':
        p.error(f'Expected official BakkesMod 228, found {version}')
    with zipfile.ZipFile(a.address_library) as archive:
        matches = [n for n in archive.namelist() if n.endswith('versionlib-1-7-104-0.bin')]
        if len(matches) != 1:
            p.error('Archive must contain the Address Library for 1.7.104.0')
        library = archive.read(matches[0])
        if struct.unpack_from('<5I', library) != (5, *SKYRIM_VERSION):
            p.error('Address Library header does not match the current game')
    install = Installer()
    plugins = sky/'Data/SKSE/Plugins'
    install.put(plugins/'versionlib-1-7-104-0.bin', library)
    install.copy(ROOT/'build-win/skse-plugin/SkyrimRocketBridge.dll', plugins/'SkyrimRocketBridge.dll')
    if not (plugins/'SkyrimRocketBridge.ini').exists():
        install.copy(ROOT/'skse-plugin/SkyrimRocketBridge.ini', plugins/'SkyrimRocketBridge.ini')
    bm = apps/'compatdata/252950/pfx/drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod'
    ini=plugins/'SkyrimRocketBridge.ini'
    text=ini.read_text()
    settings={'NativeTerrain':'1','TerrainMeshPath':'Z:'+str(bm/'data/rocket-skyrim-terrain.rltm').replace('/','\\')}
    for key,value in settings.items():
        pattern=r'^'+re.escape(key)+r'=.*$'
        if re.search(pattern,text,re.M):text=re.sub(pattern,lambda _:key+'='+value,text,flags=re.M)
        else:text+='\n'+key+'='+value+'\n'
    install.put(ini,text.encode())

    # Keep any existing user's binds/configs. Update official binaries and data.
    for source in sorted((a.bakkes_runtime/'files').rglob('*')):
        if not source.is_file():
            continue
        relative = source.relative_to(a.bakkes_runtime/'files')
        target = bm/relative
        if relative.parts[0] == 'cfg' and target.exists():
            continue
        if relative.name == 'updaterinfo.txt':
            continue  # the ZIP contains obsolete 2020 metadata; injector fetches current metadata
        install.copy(source, target)
    install.copy(a.bakkes_runtime/'BakkesMod.exe', bm.parent/'BakkesMod.exe')
    install.copy(ROOT/'build-win/bakkes-plugin/RocketSkyrim.dll', bm/'plugins/RocketSkyrim.dll')
    config = bm/'cfg/plugins.cfg'
    text = config.read_text() if config.exists() else ''
    if not re.search(r'^\s*plugin\s+load\s+RocketSkyrim\s*$', text, re.M | re.I):
        install.put(config, (text.rstrip()+'\nplugin load RocketSkyrim\n').encode())
    if a.crt:
        # RL's stock DLLs must survive normal/EAC launches. The bridge launcher
        # borrows these modern DLLs from the injector directory for its session.
        for source in a.crt.glob('*.dll'):
            for destination in (sky, bm.parent):
                install.copy(source, destination/source.name)
    install.save()
    report = dict(steam=str(steam), skyrim=str(sky), bakkesmod=str(bm),
                  skyrim_version='1.7.104.0', skse_version='2.3.1', rl_build=build,
                  bakkesmod_version=version, backup_manifest=str(install.folder/'manifest.json'))
    (ROOT/'build/install-state.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
