#!/usr/bin/env python3
"""Bounded SKSE startup probe on an invisible Xvfb display; restore INIs afterward."""
import argparse
import base64
import configparser
import json
from pathlib import Path
import subprocess
import time
from isolation import VirtualDisplay, proton_command, graphics_environment, require_steam_client

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--seconds', type=int, default=45)
    p.add_argument('--demo', action='store_true', help='Enable temporary auto-start and feed synthetic poses')
    a = p.parse_args()
    if not 10 <= a.seconds <= 60:
        p.error('Probe duration must be 10–60 seconds')
    state = json.loads((ROOT/'build/install-state.json').read_text())
    steam = Path(state['steam']); game = Path(state['skyrim'])
    require_steam_client(steam)
    compat = steam/'steamapps/compatdata/489830'
    doc = compat/'pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition'
    output = ROOT/'build/probe-skse';output.mkdir(parents=True, exist_ok=True)
    originals = {}
    started=time.time()
    virtual = process = demo = None
    display = None
    try:
        if a.demo:
            path=game/'Data/SKSE/Plugins/SkyrimRocketBridge.ini'
            originals[path]=path.read_bytes()
            text=path.read_text();text=text.replace('AutoStart=0','AutoStart=1')
            if 'AutoStart=' not in text:text+='\nAutoStart=1\n'
            text+='\nTestCell=qasmoke\n'
            path.write_text(text)
        for name, source in [('Skyrim.ini', game/'Skyrim_Default.ini'), ('SkyrimPrefs.ini', game/'Skyrim/SkyrimPrefs.ini')]:
            path = doc/name;doc.mkdir(parents=True, exist_ok=True)
            originals[path] = path.read_bytes() if path.exists() else None
            # Persist before mutation so an interrupted probe is recoverable.
            (output/'original-inis.json').write_text(json.dumps({str(k):base64.b64encode(v).decode() if v is not None else None for k,v in originals.items()},indent=2)+'\n')
            config = configparser.ConfigParser(interpolation=None, strict=False)
            config.optionxform = str
            config.read_string((originals[path] or source.read_bytes()).decode('utf-8-sig'))
            if name == 'Skyrim.ini':
                if not config.has_section('General'):config.add_section('General')
                # Enter the disposable cell only through the plugin after
                # DataLoaded; startup console commands run too early.
                config['General'].pop('sStartingCell',None)
                config['General'].pop('sStartingConsoleCommand',None)
                config['General']['sIntroSequence']=''
                config['General']['bAlwaysActive']='1'
                if not config.has_section('SaveGame'):config.add_section('SaveGame')
                config['SaveGame'].update({k:'0' for k in ('bSaveOnPause','bSaveOnTravel','bSaveOnWait','bSaveOnRest')})
            else:
                if not config.has_section('Display'):config.add_section('Display')
                config['Display'].update({'bFull Screen':'0','bBorderless':'0','iSize W':'640','iSize H':'480'})
            with path.open('w') as f:config.write(f, space_around_delimiters=False)
        with (output/'xvfb.log').open('w') as log:
            virtual=VirtualDisplay(log,'800x600x24')
        display=virtual.display
        env = virtual.environment()
        env.update(DISPLAY=display, STEAM_COMPAT_DATA_PATH=str(compat),
                   STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam), SteamAppId='489830', SteamGameId='489830',
                   PROTON_LOG='1', PROTON_LOG_DIR=str(output),
                   WINEDLLOVERRIDES='msvcp140,msvcp140_1,msvcp140_2,msvcp140_atomic_wait,vcruntime140,vcruntime140_1=n,b')
        graphics_environment(env)
        with (output/'launch.log').open('w') as log:
            process = subprocess.Popen(proton_command(steam,489830,game/'skse64_loader.exe','-waitforclose'), cwd=game, env=env,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            print(f'SKSE probe on virtual display {display}; no desktop windows', flush=True)
            if a.demo:
                demo=subprocess.Popen(['python3',str(ROOT/'tools/bridge.py'),'demo','--seconds',str(a.seconds)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
            # SKSE's loader exits after spawning SkyrimSE.exe. Its exit is not
            # the game's exit; keep the owned display alive for the full probe.
            time.sleep(a.seconds)
    finally:
        if demo:
            demo.terminate();demo.wait(timeout=5)
        # A display number may also be in the user's environment. Require our
        # unique child-only marker; never select by DISPLAY or process name alone.
        if virtual:virtual.close()
        if process:
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill()
        for path, content in originals.items():
            if content is None:path.unlink(missing_ok=True)
            else:path.write_bytes(content)
        fresh=False
        for source in (doc/'SKSE').glob('*.log'):
            if source.stat().st_mtime>=started:
                (output/source.name).write_bytes(source.read_bytes())
                if source.name=='SkyrimRocketBridge.log':fresh=True
        plugin_log = output/'SkyrimRocketBridge.log'
        text = plugin_log.read_text() if plugin_log.exists() else ''
        result = dict(loaded=fresh and 'Listening on 127.0.0.1:29741' in text,
                      models_and_frames_verified=fresh and 'Replay frame ' in text,
                      notes='Startup and hook installation only; rendering/input still require live validation',
                      log=str(plugin_log))
        (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2), flush=True)
    if not result['loaded']:raise SystemExit(1)

if __name__ == '__main__':main()
