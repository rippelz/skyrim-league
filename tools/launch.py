#!/usr/bin/env python3
"""Run each game on its own display and expose both in one local browser panel."""
import argparse
import configparser
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import shutil
import subprocess
import time
from contextlib import ExitStack
from isolation import DesktopDisplay,VirtualDisplay,graphics_environment,proton_command,require_steam_client
from prepare_viewer import prepare
from temporary_files import TemporaryFiles,stage_rl_runtime

ROOT=Path(__file__).resolve().parents[1]
CRT='msvcp140,msvcp140_1,msvcp140_2,msvcp140_atomic_wait,vcruntime140,vcruntime140_1=n,b'

def environment(steam,appid,display,logs):
    env=graphics_environment(display.environment())
    if isinstance(display,DesktopDisplay):env.pop('MESA_VK_WSI_DEBUG',None)
    env.update(STEAM_COMPAT_DATA_PATH=str(steam/f'steamapps/compatdata/{appid}'),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam),STEAM_COMPAT_APP_ID=str(appid),
               SteamAppId=str(appid),SteamGameId=str(appid),SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='1',PROTON_USE_SDL='1',PROTON_LOG='1',PROTON_LOG_DIR=str(logs),WINEDLLOVERRIDES=CRT)
    return env


def port_free(port):
    with socket.socket() as s:
        s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        s.bind(('127.0.0.1',port))

def family_sharing_refused(steam):
    log=steam/'logs/console_log.txt'
    if not log.exists():return False
    lines=log.read_text(errors='replace').splitlines()
    failure=success=-1
    for i,line in enumerate(lines):
        if 'AppID 489830' not in line:continue
        if 'LaunchApp failed' in line:failure=i
        if 'Game process added' in line:success=i
    return failure>success and 'FamilySharing' in lines[failure]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--demo',action='store_true',help='Synthetic poses instead of Rocket League')
    p.add_argument('--replay',type=Path,help='Replay a recording into Skyrim instead of starting Rocket League')
    p.add_argument('--manual-bridge',action='store_true',help='Wait for F8 before entering the bridge')
    p.add_argument('--no-rl',action='store_true',help='Do not launch Rocket League')
    p.add_argument('--desktop-rl',action='store_true',help='Open Rocket League on your desktop')
    for game in ('rl','skyrim'):
        p.add_argument('--'+game+'-workspace',type=int,help='Desktop workspace number')
        p.add_argument('--'+game+'-monitor',help='Desktop monitor name')
    p.add_argument('--desktop-skyrim',action='store_true',help='Open Skyrim directly on your desktop; keep RL on a private display')
    p.add_argument('--gamescope-rl',action='store_true',help='Give Rocket League a nested GPU compositor on its private display')
    p.add_argument('--gamescope-skyrim',action='store_true',help='Give Skyrim a nested GPU compositor on its private display')
    p.add_argument('--vanilla',action='store_true',help='Run Skyrim’s normal launcher alone for first-run setup, without SKSE')
    p.add_argument('--no-skyrim',action='store_true',help='RL setup/recording only')
    p.add_argument('--viewer-only',action='store_true',help='Test isolated displays/viewer without either game')
    p.add_argument('--official-injector',action='store_true',help='Use the official GUI instead of the narrow offline Proton loader')
    p.add_argument('--retry-skyrim',action='store_true',help='Retry Skyrim after fixing the recorded Steam borrowing refusal')
    p.add_argument('--seconds',type=float,help='Stop this session after a bounded duration')
    p.add_argument('--dry-run',action='store_true')
    a=p.parse_args()
    if a.desktop_skyrim and (a.no_skyrim or a.viewer_only or a.gamescope_skyrim):p.error('--desktop-skyrim needs Skyrim and cannot use --viewer-only or --gamescope-skyrim')
    if a.no_rl and a.no_skyrim:p.error('Choose at least one game')
    if a.desktop_rl and (a.no_rl or a.demo or a.replay or a.vanilla or a.viewer_only or a.gamescope_rl):p.error('--desktop-rl needs RL and cannot use a private gamescope')
    if a.replay:
        if a.demo or a.no_skyrim:p.error('--replay requires Skyrim and cannot be combined with --demo')
        if not a.replay.is_file():p.error('Replay recording not found')
    if a.vanilla and (a.demo or a.replay or a.no_skyrim):p.error('--vanilla runs Skyrim’s normal launcher alone')
    if (a.gamescope_skyrim or a.gamescope_rl) and not shutil.which('gamescope'):p.error('gamescope is not installed')
    state=json.loads((ROOT/'build/install-state.json').read_text());steam=Path(state['steam']);sky=Path(state['skyrim']);bm=Path(state['bakkesmod'])
    x11vnc=ROOT/'.deps/viewer/usr/bin/x11vnc';python=ROOT/'.deps/viewer-env/bin/python';client=ROOT/'.deps/noVNC'
    if a.dry_run:
        rl=None if a.no_rl or a.demo or a.replay or a.vanilla or a.viewer_only else proton_command(steam,252950,steam/'steamapps/common/rocketleague/Binaries/Win64/RocketLeague.exe','-NoEAC')
        skyrim=None if a.no_skyrim or a.viewer_only else (proton_command(steam,489830,sky/'SkyrimSELauncher.exe') if a.vanilla else proton_command(steam,489830,sky/'skse64_loader.exe','-waitforclose'))
        print(json.dumps(dict(rl=rl,skyrim=skyrim,replay=str(a.replay.resolve()) if a.replay else None,desktop_skyrim=a.desktop_skyrim,gamescope_skyrim=a.gamescope_skyrim,viewer_ready=x11vnc.is_file() and python.is_file() and client.is_dir(),browser_opens_automatically=False),indent=2));return
    if not x11vnc.is_file() or not python.is_file() or not client.is_dir():p.error('Run tools/fetch_viewer.py first')
    prepare(client)
    if not a.viewer_only:require_steam_client(steam)
    if not a.no_skyrim and not a.viewer_only and not a.retry_skyrim and family_sharing_refused(steam):
        p.error('Steam last refused Skyrim with FamilySharing. Use --no-skyrim for RL setup; after borrowing works, use --retry-skyrim. No game or Steam dialog was opened.')
    if not a.no_rl and not a.demo and not a.replay and not a.vanilla and not a.viewer_only:
        build=re.search(r'"buildid"\s+"(\d+)"',(steam/'steamapps/appmanifest_252950.acf').read_text()).group(1)
        if build not in ('25400034','25535926') or (bm/'version.txt').read_text().strip()!='228':p.error('Game/BakkesMod versions changed; verify compatibility before loading')
        if not a.official_injector and not (ROOT/'build-win/BridgeInjector.exe').is_file():p.error('Rebuild the offline loader first')
    for proc in Path('/proc').iterdir():
        if not proc.name.isdecimal():continue
        try:
            command=(proc/'cmdline').read_bytes().split(b'\0')[0].lower()
            if b'rocketleague.exe' in command or b'skyrimse.exe' in command:p.error('Close the existing game session before launching another copy')
        except OSError:pass
    if a.seconds is not None and a.seconds<=0:p.error('--seconds must be positive')
    for port in (6081,6082,5901,5902):port_free(port)
    folder=ROOT/'build/sessions'/time.strftime('%Y%m%d-%H%M%S');folder.mkdir(parents=True,exist_ok=False)
    lock=(ROOT/'build/session.lock').open('w');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    temporary=TemporaryFiles(folder);processes=[];displays=[];started=time.monotonic()
    stop=False
    def request_stop(*_):
        nonlocal stop
        stop=True
    signal.signal(signal.SIGINT,request_stop);signal.signal(signal.SIGTERM,request_stop)
    password=secrets.token_hex(4)
    with ExitStack() as stack:
        try:
            ld=os.environ.get('LD_LIBRARY_PATH','')
            viewer_env=os.environ.copy();viewer_env['LD_LIBRARY_PATH']=str(ROOT/'.deps/viewer/usr/lib')+(':'+ld if ld else '')
            auth=folder/'viewer-password';auth.touch(mode=0o600)
            subprocess.run([str(x11vnc),'-storepasswd',password,str(auth)],env=viewer_env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
            panels=[]
            for name,appid,rfb_port,web_port in [('rl',252950,5901,6081),('skyrim',489830,5902,6082)]:
                if name=='rl' and (a.no_rl or a.demo or a.replay or a.vanilla):continue
                if name=='skyrim' and a.no_skyrim:continue
                logs=folder/name;logs.mkdir()
                log=stack.enter_context((logs/'launch.log').open('w'))
                desktop=a.desktop_skyrim if name=='skyrim' else a.desktop_rl
                if desktop:
                    workspace=getattr(a,name+'_'+'workspace');monitor=getattr(a,name+'_'+'monitor')
                    if workspace is not None and not 1<=workspace<=100:p.error('Workspace must be 1–100')
                    if shutil.which('hyprctl') and workspace is not None:
                        if monitor:
                            monitors=json.loads(subprocess.check_output(['hyprctl','monitors','-j']))
                            if monitor not in [m['name'] for m in monitors]:raise ValueError('Monitor is no longer connected')
                            rule='hl.workspace_rule({workspace='+json.dumps(str(workspace))+',monitor='+json.dumps(monitor)+'})'
                            subprocess.run(['hyprctl','eval',rule],check=True,stdout=subprocess.DEVNULL)
                        rule='hl.window_rule({name="rocketbridge-ui-'+name+'",match={class="steam_app_'+str(appid)+'"},workspace='+json.dumps(str(workspace)+' silent')+',no_initial_focus=true,focus_on_activate=false})'
                        subprocess.run(['hyprctl','eval',rule],check=True,stdout=subprocess.DEVNULL)
                display=DesktopDisplay() if desktop else VirtualDisplay(log);displays.append(display)
                if not desktop:
                    server_env=display.environment();server_env['LD_LIBRARY_PATH']=viewer_env['LD_LIBRARY_PATH']
                    server=subprocess.Popen([str(x11vnc),'-display',display.display,'-rfbport',str(rfb_port),'-localhost','-rfbauth',str(auth),'-forever','-shared','-noxdamage','-nowf','-quiet'],env=server_env,stdout=log,stderr=subprocess.STDOUT)
                    processes.append(server)
                    proxy=subprocess.Popen([str(python),'-m','websockify','--web',str(client),f'127.0.0.1:{web_port}',f'127.0.0.1:{rfb_port}'],env=viewer_env,stdout=log,stderr=subprocess.STDOUT)
                    processes.append(proxy)
                    panels.append(dict(name=name,port=web_port))
                if a.viewer_only:continue
                env=environment(steam,appid,display,logs)
                if name=='rl':
                    stage_rl_runtime(temporary,bm.parent,steam/'steamapps/common/rocketleague/Binaries/Win64')
                    config=bm/'cfg/plugins.cfg'
                    commands=config.read_text().rstrip()
                    if 'plugin load RocketSkyrim' not in commands:commands+='\nplugin load RocketSkyrim'
                    temporary.put(config,(commands+'\nsleep 5000; sb_start\n').encode())
                    game=steam/'steamapps/common/rocketleague'
                    settings=steam/'steamapps/compatdata/252950/pfx/drive_c/users/steamuser/Documents/My Games/Rocket League/TAGame/Config/TASystemSettings.ini'
                    if settings.exists():
                        temporary.put(settings,re.sub(r'^CustomFPS=.*$', 'CustomFPS=60',settings.read_text(),flags=re.M).encode())
                    env['DXVK_FRAME_RATE']='60'
                    command=proton_command(steam,appid,game/'Binaries/Win64/RocketLeague.exe','-NoEAC','-nomovie','-windowed','-ResX=1280','-ResY=720')
                    if a.gamescope_rl:
                        env['SDL_VIDEODRIVER']='x11'
                        command=['gamescope','--backend','sdl','-W','1280','-H','720','-w','1280','-h','720','-r','60','-o','60','--','env','-u','MESA_VK_WSI_DEBUG',*command]
                    processes.append(subprocess.Popen(command,cwd=game,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True))
                    time.sleep(6)
                    # The official GUI's platform detection can fail under
                    # Proton. The narrow loader verifies the actual -NoEAC
                    # command line and refuses EAC-parented or multiple games.
                    loader=bm.parent/'BakkesMod.exe' if a.official_injector else ROOT/'build-win/BridgeInjector.exe'
                    processes.append(subprocess.Popen(proton_command(steam,appid,loader,in_prefix=True),cwd=bm.parent,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True))
                else:
                    # Skyrim's Havok simulation assumes a bounded frame rate.
                    # Compositor refresh alone does not cap DXVK rendering.
                    env['DXVK_FRAME_RATE']='60'
                    env['VKD3D_FRAME_RATE']='60'
                    ini=sky/'Data/SKSE/Plugins/SkyrimRocketBridge.ini'
                    text=ini.read_text()
                    auto_start='0' if a.manual_bridge else '1'
                    text=re.sub(r'^AutoStart=.*$',lambda _: 'AutoStart='+auto_start,text,flags=re.M)
                    if 'AutoStart=' not in text:text+='\nAutoStart='+auto_start+'\n'
                    temporary.put(ini,text.encode())
                    settings=steam/'steamapps/compatdata/489830/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/Skyrim.ini'
                    general=configparser.ConfigParser(interpolation=None,strict=False);general.optionxform=str;general.read(settings)
                    if not general.has_section('General'):general.add_section('General')
                    general['General'].update({'sIntroSequence':'','bAlwaysActive':'1'})
                    if not a.vanilla:
                        if not general.has_section('Archive'):general.add_section('Archive')
                        general['Archive'].update({'bInvalidateOlderFiles':'1','sResourceDataDirsFinal':''})
                    import io
                    data=io.StringIO();general.write(data,space_around_delimiters=False);temporary.put(settings,data.getvalue().encode())
                    prefs=steam/'steamapps/compatdata/489830/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SkyrimPrefs.ini'
                    cfg=configparser.ConfigParser(interpolation=None,strict=False);cfg.optionxform=str;cfg.read(prefs)
                    for section in ('Display','MAIN'):
                        if not cfg.has_section(section):cfg.add_section(section)
                    cfg['Display'].update({'bFull Screen':'0','bBorderless':'0','iSize W':'1280','iSize H':'720','iVSyncPresentInterval':'1'})
                    cfg['MAIN']['bGamepadEnable']='1'
                    import io
                    data=io.StringIO();cfg.write(data,space_around_delimiters=False);temporary.put(prefs,data.getvalue().encode())
                    command=proton_command(steam,appid,sky/'SkyrimSELauncher.exe') if a.vanilla else proton_command(steam,appid,sky/'skse64_loader.exe','-waitforclose')
                    if a.gamescope_skyrim:
                        env['SDL_VIDEODRIVER']='x11'
                        command=['gamescope','--backend','sdl','-W','1280','-H','720','-w','1280','-h','720','-r','60','-o','60','--','env','-u','MESA_VK_WSI_DEBUG',*command]
                    processes.append(subprocess.Popen(command,cwd=sky,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True))
            page=folder/'panel.html'
            html=(ROOT/'tools/panel.html').read_text().replace('__PANELS__',json.dumps(panels)).replace('__PASSWORD__',password)
            instructions=('Complete Skyrim’s normal first-run setup and start a new game to initialize it.' if a.vanilla else
                          'Load a Skyrim scene to view the recorded Rocket League state.' if a.replay else
                          'Load a Skyrim scene to view the synthetic bridge test.' if a.demo else
                          'Enter offline Free Play in Rocket League, then load a Skyrim scene. View the bridge in the Skyrim tab.')
            html=html.replace('__INSTRUCTIONS__',instructions)
            if a.desktop_skyrim:html=html.replace('View the bridge in the Skyrim tab.','Play in the direct Skyrim desktop window.')
            page.write_text(html);page.chmod(0o600)
            (folder/'session.json').write_text(json.dumps(dict(pid=os.getpid(),panel=str(page),panels=panels,layout=[dict(game=name,display='desktop' if (a.desktop_rl if name=='rl' else a.desktop_skyrim) else 'private',workspace=getattr(a,name+'_workspace'),monitor=getattr(a,name+'_monitor')) for name in ('rl','skyrim') if not (a.no_rl if name=='rl' else a.no_skyrim)]),indent=2)+'\n')
            print('Open this panel when ready: '+str(page),flush=True)
            print(('Desktop games open on their selected workspaces.' if a.desktop_skyrim or a.desktop_rl else 'Games stay on private displays.')+' Ctrl+C here closes only this session.',flush=True)
            if a.demo and not a.viewer_only:processes.append(subprocess.Popen(['python3',str(ROOT/'tools/bridge.py'),'demo'],stdout=stack.enter_context((folder/'demo.log').open('w')),stderr=subprocess.STDOUT))
            if a.replay and not a.viewer_only:processes.append(subprocess.Popen(['python3',str(ROOT/'tools/bridge.py'),'replay',str(a.replay.resolve()),'--loop'],stdout=stack.enter_context((folder/'replay.log').open('w')),stderr=subprocess.STDOUT,start_new_session=True))
            if not a.no_rl and not a.no_skyrim and not a.viewer_only:
                focus_command=['python3',str(ROOT/'tools/controller_focus.py'),'--launcher-pid',str(os.getpid())]
                if not a.desktop_skyrim:focus_command.append('--private-skyrim')
                processes.append(subprocess.Popen(focus_command,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL))
            while not stop and (a.seconds is None or time.monotonic()-started<a.seconds):time.sleep(.25)
        finally:
            for display in reversed(displays):display.close()
            for process in reversed(processes):
                if process.poll() is None:process.terminate()
            for process in processes:
                try:process.wait(timeout=3)
                except subprocess.TimeoutExpired:process.kill()
            temporary.restore()
            auth.unlink(missing_ok=True)
            print('Session stopped. Logs: '+str(folder),flush=True)

if __name__=='__main__':
    try:main()
    except (OSError,ValueError,RuntimeError) as e:raise SystemExit(str(e))
