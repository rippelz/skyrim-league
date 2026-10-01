#!/usr/bin/env python3
"""Native Steam/Windows session owner. Never stops Steam or pre-existing games."""
import argparse,configparser,json,os,signal,subprocess,time
from pathlib import Path
from windows_platform import ROOT,psutil_module,processes_at,identity,matching_process,place_window,monitors

def update_record(folder,data):
    tmp=folder/'session.next';tmp.write_text(json.dumps(data,indent=2));os.replace(tmp,folder/'session.json')

def stop_owned(records):
    psutil=psutil_module()
    for record in records:
        if not record.get('owned'):continue
        process=matching_process(record)
        if process:
            try:process.terminate()
            except psutil.Error:pass

def plan(state,choices):
    sky=Path(state['skyrim']);rl=Path(state['rl']);steam=Path(state['steam'])
    result={}
    if choices['rl']['enabled']:result['rl']=[str(steam/'steam.exe'),'-applaunch','252950','-NoEAC']
    if choices['skyrim']['enabled']:result['skyrim']=[str(sky/'skse64_loader.exe'),'-waitforclose']
    return result

def verify_runtime(state):
    from install import file_version,SKYRIM_VERSION,SUPPORTED_RL_BUILDS
    import re
    sky=Path(state['skyrim']);rl=Path(state['rl']);bm=Path(state['bakkesmod'])
    if file_version(sky/'SkyrimSE.exe')!=SKYRIM_VERSION:raise ValueError('Skyrim changed since setup. This bridge supports 1.7.104.0.')
    match=re.search(r'"buildid"\s+"(\d+)"',(rl.parent.parent/'appmanifest_252950.acf').read_text())
    if not match or match.group(1) not in SUPPORTED_RL_BUILDS:raise ValueError('RL changed since setup. Check bridge/BakkesMod compatibility before launch.')
    if (bm/'version.txt').read_text().strip()!='228':raise ValueError('BakkesMod changed since setup. Tested runtime: 228.')
    for path in (sky/'skse64_loader.exe',sky/'Data/SKSE/Plugins/SkyrimRocketBridge.dll',bm/'plugins/RocketSkyrim.dll'):
        if not path.is_file():raise ValueError('Missing installed dependency: '+str(path))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--choices',type=Path,help='Launcher display choices JSON')
    p.add_argument('--no-rl',action='store_true');p.add_argument('--no-skyrim',action='store_true')
    p.add_argument('--attach',action='store_true',help='Use already-running games without owning/stopping them')
    p.add_argument('--dry-run',action='store_true');a=p.parse_args()
    if os.name!='nt' and not a.dry_run:p.error('Run on Windows; launch-bridge.sh is the Linux launcher.')
    try:
        state=json.loads((ROOT/'build/install-state.json').read_text())
        if state.get('platform')!='windows':raise ValueError('Run setup-windows.cmd first.')
        choices=json.loads(a.choices.read_text()) if a.choices else {g:dict(enabled=not getattr(a,'no_'+g),display='desktop',monitor='') for g in ('rl','skyrim')}
        if set(choices)!={'rl','skyrim'}:raise ValueError('Choose both games.')
        for choice in choices.values():
            if type(choice.get('enabled')) is not bool or choice.get('display') not in ('desktop','minimized') or not isinstance(choice.get('monitor',''),str):raise ValueError('Invalid Windows display choice.')
        if not any(c['enabled'] for c in choices.values()):raise ValueError('Enable at least one game.')
        commands=plan(state,choices)
        if a.dry_run:print(json.dumps(dict(commands=commands,choices=choices),indent=2));return
        verify_runtime(state)
        import msvcrt
        lock_path=ROOT/'build/session.lock';lock_path.parent.mkdir(parents=True,exist_ok=True)
        session_lock=lock_path.open('a+b')
        if session_lock.tell()==0:session_lock.write(b'\0');session_lock.flush()
        session_lock.seek(0)
        try:msvcrt.locking(session_lock.fileno(),msvcrt.LK_NBLCK,1)
        except OSError:raise ValueError('Another Windows bridge session owns the launcher lock.')
        psutil=psutil_module()
        if not processes_at(Path(state['steam'])/'steam.exe'):raise ValueError('Open Steam and sign in before starting the games.')
        available={m['name'] for m in monitors()}
        if any(c.get('monitor') and c['monitor'] not in available for c in choices.values()):raise ValueError('Selected monitor is no longer connected.')
        sky=Path(state['skyrim']);rl=Path(state['rl'])
        executables={'rl':rl/'Binaries/Win64/RocketLeague.exe','skyrim':sky/'SkyrimSE.exe'}
        existing={g:processes_at(executables[g]) for g in commands}
        if not a.attach and any(existing.values()):raise ValueError('Close existing games first, or use --attach to leave them outside session stop controls.')
        if a.attach and any(len(v)!=1 for v in existing.values()):raise ValueError('--attach requires exactly one running copy of each selected game.')
        # Official injector stays under the user's ownership (it can manage other sessions).
        if 'rl' in commands and not a.attach and state.get('injector'):
            injector=Path(state['injector'])
            if not processes_at(injector):subprocess.Popen([str(injector)],cwd=injector.parent)
        folder=ROOT/'build/sessions'/('win-'+str(time.time_ns()));folder.mkdir(parents=True)
        data=dict(pid=os.getpid(),created=psutil.Process().create_time(),platform='windows',panels=[],
                  layout=[dict(game=g,display=c['display'],monitor=c.get('monitor',''),workspace=None) for g,c in choices.items() if c['enabled']],games=[])
        update_record(folder,data);stop=False
        def request_stop(*_):
            nonlocal stop
            stop=True
        signal.signal(signal.SIGINT,request_stop);signal.signal(signal.SIGTERM,request_stop)
        children=[]
        try:
            launched=time.time();pending=set(commands);placed=set()
            for game,command in commands.items():
                if a.attach:
                    record=identity(existing[game][0]);record.update(game=game,owned=False);data['games'].append(record);pending.remove(game)
                else:
                    cwd=sky if game=='skyrim' else Path(state['steam'])
                    env=os.environ.copy();env['SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS']='1'
                    if game=='skyrim':env.update(SteamAppId='489830',SteamGameId='489830')
                    children.append(subprocess.Popen(command,cwd=cwd,env=env))
                    print('Launching '+game,flush=True)
            update_record(folder,data)
            while not stop and not (folder/'stop.request').exists():
                for game in list(pending):
                    candidates=[proc for proc in processes_at(executables[game]) if proc.create_time()>=launched-1]
                    if len(candidates)>1:raise ValueError('Multiple new '+game+' copies detected; cannot identify an owned game.')
                    if candidates:
                        record=identity(candidates[0]);record.update(game=game,owned=True);data['games'].append(record);pending.remove(game);update_record(folder,data)
                        print('Started '+game+' PID '+str(record['pid']),flush=True)
                for record in data['games']:
                    if record['game'] not in placed and matching_process(record):
                        c=choices[record['game']]
                        if place_window(record['pid'],c['display'],c.get('monitor','')):placed.add(record['game'])
                if pending and time.time()-launched>120:raise ValueError('Game startup timed out: '+', '.join(pending)+'. Check Steam ownership and launch logs.')
                if not pending and not any(matching_process(r) for r in data['games']):break
                time.sleep(.25)
        finally:
            stop_owned(data['games'])
            # The SKSE loader belongs to us; Steam and the official injector do not.
            for child in children:
                if child.args[0].lower().endswith('skse64_loader.exe') and child.poll() is None:child.terminate()
            data['stopped']=time.time();update_record(folder,data)
            print('Session stopped; Steam, BakkesMod and attached games left running.',flush=True)
    except (OSError,ValueError,KeyError,RuntimeError) as error:p.error(str(error))
if __name__=='__main__':main()
