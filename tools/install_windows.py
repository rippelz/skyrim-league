#!/usr/bin/env python3
"""Install Skyrim League on Windows; discover separate Steam libraries and preserve settings."""
import argparse,json,os,re,struct,sys,zipfile
from pathlib import Path
from install import Installer,file_version,SKYRIM_VERSION,SUPPORTED_RL_BUILDS
from windows_platform import steam_root,find_game,bakkes_root,processes_at
ROOT=Path(__file__).resolve().parents[1]

def ini_settings(text,settings):
    for key,value in settings.items():
        pattern=r'(?mi)^'+re.escape(key)+r'\s*=.*$'
        if re.search(pattern,text):text=re.sub(pattern,lambda _:key+'='+str(value),text)
        else:text+='\n'+key+'='+str(value)+'\n'
    return text

def asset_files(data):
    data=Path(data)
    required=['SKSE/Plugins/RocketBridge-Car.rmesh','SKSE/Plugins/RocketBridge-Ball.rmesh',
              'meshes/rocketbridge/fennec.nif','meshes/rocketbridge/ball.nif','textures/rocketbridge/boost_flame.dds']
    for name in required:
        if not (data/name).is_file():raise ValueError('Missing local asset: '+str(data/name))
    # Validate actual material references, so a transferred mesh cannot silently
    # arrive without the diffuse/normal/paint maps that make it recognizable.
    texture_names={'textures/rocketbridge/flat_n.dds'}
    for mesh in required[:2]:
        raw=(data/mesh).read_bytes()
        if len(raw)<8:raise ValueError('Truncated render mesh: '+mesh)
        magic,count=struct.unpack_from('<II',raw)
        if magic!=0x314D4252 or not 1<=count<=32:raise ValueError('Invalid render mesh: '+mesh)
        at=8
        for _ in range(count):
            if at+4>len(raw):raise ValueError('Truncated material: '+mesh)
            length=struct.unpack_from('<I',raw,at)[0];at+=4
            if not 1<=length<=256 or at+length+4>len(raw):raise ValueError('Invalid material path: '+mesh)
            name=raw[at:at+length].decode('utf-8').replace('\\','/').lower();at+=length
            if not name.startswith('textures/rocketbridge/') or '..' in name:raise ValueError('Invalid texture path: '+name)
            vertices=struct.unpack_from('<I',raw,at)[0];at+=4
            if not vertices or vertices>500000 or vertices%3 or at+vertices*48>len(raw):raise ValueError('Invalid vertices: '+mesh)
            at+=vertices*48;texture_names.add(name)
            if 'body_grain_d' in name:
                texture_names.update(('textures/rocketbridge/body_grain_bodynormalmap.dds','textures/rocketbridge/body_grain_blankskin.dds'))
            elif name.endswith('_d.dds'):texture_names.add(name[:-6]+'_n.dds')
        if at!=len(raw):raise ValueError('Trailing render mesh bytes: '+mesh)
    for name in texture_names:
        if not (data/name).is_file():raise ValueError('Missing material map: '+str(data/name))
    result=[p for directory in ('meshes/rocketbridge','textures/rocketbridge') for p in (data/directory).rglob('*') if p.is_file()]
    return result+[data/name for name in required[:2]]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--steam',type=Path);p.add_argument('--skyrim',type=Path);p.add_argument('--rl',type=Path)
    p.add_argument('--bakkesmod',type=Path);p.add_argument('--injector',type=Path,help='Official BakkesMod.exe (or leave it already running)')
    p.add_argument('--address-library',type=Path,help='Address Library ZIP; optional if matching library already installed')
    p.add_argument('--assets',type=Path,help='Private Skyrim Data asset folder from export_local_assets.py')
    p.add_argument('--dry-run',action='store_true');a=p.parse_args()
    if os.name!='nt':p.error('Run this installer on Windows. Use tools/install.py for Linux.')
    try:
        steam=(a.steam or steam_root()).resolve()
        sky=(a.skyrim or find_game(steam,489830)[0]).resolve()
        if a.rl:
            rl=a.rl.resolve();manifest=rl.parent.parent/'appmanifest_252950.acf'
            match=re.search(r'"buildid"\s+"(\d+)"',manifest.read_text())
            build=match.group(1) if match else ''
        else:rl,build=find_game(steam,252950)
        bm=(a.bakkesmod or bakkes_root()).resolve()
        if file_version(sky/'SkyrimSE.exe')!=SKYRIM_VERSION:raise ValueError('Requires Skyrim 1.7.104.0 with matching SKSE 2.3.1.')
        if not (sky/'skse64_loader.exe').is_file() or not (sky/'skse64_1_7_104.dll').is_file():raise ValueError('Install matching SKSE first from https://skse.silverlock.org/')
        if not (rl/'Binaries/Win64/RocketLeague.exe').is_file():raise ValueError('RocketLeague.exe not found.')
        if build not in SUPPORTED_RL_BUILDS:raise ValueError('RL build '+build+' is outside the tested bridge versions.')
        if not (bm/'version.txt').is_file() or (bm/'version.txt').read_text().strip()!='228':raise ValueError('Install the official compatible BakkesMod runtime (tested: 228) first.')
        plugins=sky/'Data/SKSE/Plugins';library_name='versionlib-1-7-104-0.bin'
        if a.address_library:
            with zipfile.ZipFile(a.address_library) as z:
                matches=[name for name in z.namelist() if name.endswith(library_name)]
                if len(matches)!=1:raise ValueError('Address Library archive does not contain one matching library.')
                library=z.read(matches[0])
        else:library=(plugins/library_name).read_bytes()
        if len(library)<20 or struct.unpack_from('<5I',library)!=(5,*SKYRIM_VERSION):raise ValueError('Address Library does not match Skyrim.')
        dlls={plugins/'SkyrimRocketBridge.dll':ROOT/'build-win/skse-plugin/SkyrimRocketBridge.dll',bm/'plugins/RocketSkyrim.dll':ROOT/'build-win/bakkes-plugin/RocketSkyrim.dll'}
        for dll in dlls.values():
            if not dll.is_file():raise ValueError('Missing built plugin: '+str(dll))
        assets=asset_files(a.assets or sky/'Data')
        injector=a.injector.resolve() if a.injector else None
        if injector and not injector.is_file():raise ValueError('Official BakkesMod injector not found.')
        report=dict(platform='windows',steam=str(steam),skyrim=str(sky),rl=str(rl),bakkesmod=str(bm),injector=str(injector) if injector else None,rl_build=build,skyrim_version='1.7.104.0',bakkesmod_version='228')
        if a.dry_run:print(json.dumps(report,indent=2));return
        if processes_at(sky/'SkyrimSE.exe') or processes_at(rl/'Binaries/Win64/RocketLeague.exe'):raise ValueError('Close both games before installing DLLs.')
        install=Installer()
        for target,source in dlls.items():install.copy(source,target)
        install.put(plugins/library_name,library)
        ini=plugins/'SkyrimRocketBridge.ini'
        text=ini.read_text() if ini.exists() else (ROOT/'skse-plugin/SkyrimRocketBridge.ini').read_text()
        settings=dict(NativeTerrain=1,TerrainCollisions=0,TerrainMeshPath=str(bm/'data/rocket-skyrim-terrain.rltm'))
        if not ini.exists():settings.update(CarModel=r'rocketbridge\fennec.nif',BallModel=r'rocketbridge\ball.nif',CarScale=1,BallScale=1,RenderBrightness=1,SuppressSurvivalPrompt=1)
        install.put(ini,ini_settings(text,settings).encode())
        asset_root=Path(a.assets or sky/'Data')
        for source in assets:install.copy(source,sky/'Data'/source.relative_to(asset_root))
        cfg=bm/'cfg/plugins.cfg';text=cfg.read_text() if cfg.exists() else ''
        if not re.search(r'^\s*plugin\s+load\s+RocketSkyrim\s*$',text,re.M|re.I):install.put(cfg,(text.rstrip()+'\nplugin load RocketSkyrim\n').encode())
        # Optional app-local redistributable files from the Windows bundle.
        for source in (ROOT/'runtime').glob('*.dll'):
            for folder in (sky,rl/'Binaries/Win64'):install.copy(source,folder/source.name)
        install.save();report['backup_manifest']=str(install.folder/'manifest.json')
        (ROOT/'build/install-state.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2));print('Installed. Open bridge-ui.cmd to manage Skyrim League.')
    except (OSError,ValueError,KeyError,RuntimeError,zipfile.BadZipFile) as error:p.error(str(error))
if __name__=='__main__':main()
