#!/usr/bin/env python3
"""Probe the official BakkesMod injector and installed RL on a private virtual display."""
import argparse
import json
from pathlib import Path
import subprocess
import time
from launch import TemporaryFiles
from isolation import VirtualDisplay, proton_command, graphics_environment, require_steam_client

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--seconds',type=int,default=55);p.add_argument('--direct-loader',action='store_true');p.add_argument('--freeplay',action='store_true',help='Enter offline freeplay through the bridge plugin and record live UDP');a=p.parse_args()
    if not 20<=a.seconds<=60:p.error('Probe duration must be 20–60 seconds')
    state=json.loads((ROOT/'build/install-state.json').read_text());steam=Path(state['steam'])
    require_steam_client(steam)
    rl=steam/'steamapps/common/rocketleague';bm=Path(state['bakkesmod']);out=ROOT/'build/probe-rl';out.mkdir(parents=True,exist_ok=True)
    display=None;processes=[];capture=None;temporary=None;diagnostics=dict(game_found_by_linux_marker=False,bakkesmod_found_in_linux_maps=False)
    for folder in ('game','injector'):(out/folder).mkdir(exist_ok=True)
    started=time.time()
    try:
        if a.freeplay:
            folder=out/('settings-'+time.strftime('%Y%m%d-%H%M%S'));folder.mkdir()
            temporary=TemporaryFiles(folder);cfg=bm/'cfg/plugins.cfg'
            commands=cfg.read_text().rstrip()
            if 'plugin load RocketSkyrim' not in commands:commands+='\nplugin load RocketSkyrim'
            temporary.put(cfg,(commands+'\nsb_start\nsleep 5000; sb_freeplay\n').encode())
            recording=out/('live-'+time.strftime('%Y%m%d-%H%M%S')+'.rlsb')
            capture=subprocess.Popen(['python3',str(ROOT/'tools/bridge.py'),'capture','--seconds',str(a.seconds),'--output',str(recording)],stdout=(out/'capture.log').open('w'),stderr=subprocess.STDOUT)
        with (out/'xvfb.log').open('w') as log:display=VirtualDisplay(log)
        env=graphics_environment(display.environment())
        env.update(STEAM_COMPAT_DATA_PATH=str(steam/'steamapps/compatdata/252950'),
                   STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam),SteamAppId='252950',SteamGameId='252950',
                   PROTON_LOG='1',PROTON_LOG_DIR=str(out),
                   WINEDLLOVERRIDES='msvcp140,msvcp140_1,msvcp140_2,msvcp140_atomic_wait,vcruntime140,vcruntime140_1=n,b')
        with (out/'launch.log').open('w') as log:
            # Plain RL executable is the offline/non-EAC entry point. Never start
            # RocketLeague_EAC.exe or change any anti-cheat installation.
            env['PROTON_LOG_DIR']=str(out/'game')
            game=subprocess.Popen(proton_command(steam,252950,rl/'Binaries/Win64/RocketLeague.exe','-NoEAC','-nomovie','-windowed','-ResX=640','-ResY=480'),
                                  cwd=rl,env=env,stdout=log,stderr=subprocess.STDOUT)
            processes.append(game);time.sleep(6)
            env['PROTON_LOG_DIR']=str(out/'injector')
            injector_path=ROOT/'build-win/BridgeInjector.exe' if a.direct_loader else bm.parent/'BakkesMod.exe'
            # A second steam.exe shim would replace the game's Steam process
            # registration and can end the Wine session when the loader exits.
            injector=subprocess.Popen(proton_command(steam,252950,injector_path,in_prefix=True),cwd=bm.parent,env=env,stdout=log,stderr=subprocess.STDOUT)
            processes.append(injector)
            print('RL and BakkesMod loader running on '+display.display,flush=True)
            time.sleep(a.seconds-6)
            for proc in Path('/proc').iterdir():
                if not proc.name.isdecimal():continue
                try:
                    if ('ROCKET_SKYRIM_ISOLATED='+display.marker).encode() in (proc/'environ').read_bytes().split(b'\0'):
                        modules=(proc/'maps').read_text(errors='replace').lower()
                        if 'rocketleague.exe' in modules:
                            diagnostics['game_found_by_linux_marker']=True
                            diagnostics['bakkesmod_found_in_linux_maps']='bakkesmod.dll' in modules
                except OSError:pass
    finally:
        if display:display.close()
        if capture:
            try:capture.wait(timeout=3)
            except subprocess.TimeoutExpired:capture.terminate();capture.wait(timeout=3)
        if temporary:temporary.restore()
        for process in processes:
            try:process.wait(timeout=3)
            except subprocess.TimeoutExpired:process.kill()
        for source in bm.parent.rglob('*.log'):
            if source.stat().st_mtime>=started:(out/source.name).write_bytes(source.read_bytes())
        injector_log=bm.parents[2]/'Local/Temp/injectorlog.log'
        if injector_log.exists() and injector_log.stat().st_mtime>=started:(out/'injectorlog.log').write_bytes(injector_log.read_bytes())
        source=bm/'bakkesmod.log'
        text=source.read_text(errors='replace') if source.exists() and source.stat().st_mtime>=started else ''
        loader_log=bm/'bridge-loader.log'
        loader=loader_log.read_text(errors='replace') if loader_log.exists() and loader_log.stat().st_mtime>=started else ''
        result=dict(bakkesmod_initialized='BakkesMod initialized' in text,
                    plugin_loaded='RocketSkyrim loaded.' in text,
                    direct_loader_completed='LoadLibraryW completed' in loader,
                    log=str(out/'bakkesmod.log'),**diagnostics)
        if a.freeplay:
            from protocol import frames
            with recording.open('rb') as source:packets=list(frames(source))
            result.update(freeplay_packets=len(packets),recording=str(recording))
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)
    if not result['plugin_loaded']:raise SystemExit(1)

if __name__=='__main__':main()
