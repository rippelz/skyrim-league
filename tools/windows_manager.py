"""Windows session backend for the shared browser settings UI."""
import json,os,subprocess,sys,time
from pathlib import Path
from manager import Manager
from windows_platform import ROOT,psutil_module,script_command,monitors
class WindowsManager(Manager):
    def instances(self):
        result=[];known=set()
        psutil=psutil_module()
        for folder in sorted((ROOT/'build/sessions').glob('win-*'),reverse=True):
            try:
                data=json.loads((folder/'session.json').read_text());pid=int(data['pid']);known.add(pid)
                active=bool(script_command(pid,'launch_windows.py'))
                if active:active=abs(psutil.Process(pid).create_time()-data['created'])<.001
                if active or len(result)<8:result.append(dict(id=folder.name,pid=pid,active=active,stopping=active and (folder/'stop.request').exists(),layout=data.get('layout'),desktop=True,panel=False))
            except (OSError,ValueError,KeyError,psutil.Error):pass
        if self.child:
            if self.child.poll() is None and self.child.pid not in known:result.insert(0,dict(id='Starting',pid=self.child.pid,active=True,stopping=False,desktop=True,panel=False))
        return result
    def monitors(self):return monitors()
    def status(self):
        data=super().status();data['capabilities']=dict(displays=['desktop','minimized'],workspaces=False,platform='windows');return data
    def start(self,data):
        if any(x['active'] for x in self.instances()):raise ValueError('Stop the current bridge session first.')
        choices=data.get('games')
        if not isinstance(choices,dict) or set(choices)!={'rl','skyrim'}:raise ValueError('Choose settings for both games.')
        available={m['name'] for m in self.monitors()}
        cleaned={}
        for game,choice in choices.items():
            if not isinstance(choice,dict) or type(choice.get('enabled')) is not bool or choice.get('display') not in ('desktop','minimized'):raise ValueError('Choose a native Windows display mode.')
            monitor=choice.get('monitor','')
            if not isinstance(monitor,str) or (monitor and monitor not in available):raise ValueError('Choose a connected monitor.')
            cleaned[game]=dict(enabled=choice['enabled'],display=choice['display'],monitor=monitor)
        if not any(c['enabled'] for c in cleaned.values()):raise ValueError('Enable at least one game.')
        folder=ROOT/'build/launch-choices';folder.mkdir(parents=True,exist_ok=True)
        file=folder/(str(time.time_ns())+'.json');file.write_text(json.dumps(cleaned))
        args=[sys.executable,str(ROOT/'tools/launch_windows.py'),'--choices',str(file)]
        self.log.parent.mkdir(parents=True,exist_ok=True)
        with self.log.open('w') as log:self.child=subprocess.Popen(args,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,creationflags=0x08000000) # CREATE_NO_WINDOW
        return dict(pid=self.child.pid)
    def stop(self,data):
        pid=int(data.get('pid',0));psutil=psutil_module()
        if not script_command(pid,'launch_windows.py'):raise ValueError('That session has already stopped.')
        for folder in (ROOT/'build/sessions').glob('win-*'):
            try:
                record=json.loads((folder/'session.json').read_text())
                if record['pid']==pid and abs(psutil_module().Process(pid).create_time()-record['created'])<.001:
                    (folder/'stop.request').touch();self.stopping.add(pid);return dict(stopping=pid)
            except (OSError,KeyError,ValueError,psutil.Error):pass
        raise ValueError('Session is still starting. Try stop again in a moment.')
