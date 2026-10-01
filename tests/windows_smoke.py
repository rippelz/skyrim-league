"""Run with native Windows Python (also runnable headlessly under Wine), no games."""
import json,os,shutil,subprocess,sys,tempfile,time,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from windows_platform import psutil_module,monitors,identity,matching_process

def main():
    api=psutil_module();record=identity(api.Process())
    assert matching_process(record).pid==os.getpid()
    assert isinstance(monitors(),list)
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory);(root/'tools').mkdir();(root/'build').mkdir();sky=root/'Skyrim'
        ini=sky/'Data/SKSE/Plugins/SkyrimRocketBridge.ini';ini.parent.mkdir(parents=True);ini.write_text('[Bridge]\n')
        for name in ('manager.py','windows_manager.py','windows_platform.py','manager.html'):shutil.copy2(ROOT/'tools'/name,root/'tools'/name)
        (root/'build/install-state.json').write_text(json.dumps(dict(skyrim=str(sky),platform='windows')))
        wrapper="import runpy,sys;sys.path.insert(0,sys.argv[1]);sys.argv=sys.argv[2:];runpy.run_path(sys.argv[0],run_name='__main__')"
        log=(root/'manager.log').open('w')
        child=subprocess.Popen([sys.executable,'-c',wrapper,str(root/'tools'),str(root/'tools/manager.py'),'--port','18767'],stdout=log,stderr=subprocess.STDOUT,creationflags=0x08000000)
        try:
            deadline=time.monotonic()+10
            while time.monotonic()<deadline and not (root/'build/manager.json').exists():
                if child.poll() is not None:
                    log.flush();raise RuntimeError('Windows manager exited early: '+(root/'manager.log').read_text())
                time.sleep(.1)
            stored=json.loads((root/'build/manager.json').read_text());url,token=stored['url'].split('#')
            request=urllib.request.Request(url+'api/status',headers={'X-Bridge-Token':token})
            data=json.load(urllib.request.urlopen(request,timeout=3))
            assert data['capabilities']['platform']=='windows' and data['capabilities']['displays']==['desktop','minimized']
            assert not data['busy'] and 'RenderBrightness' in data['settings']
            from windows_platform import script_command
            # Separate copied root uses its own script identity; verify native command APIs here.
            assert any('manager.py' in arg for arg in api.Process(child.pid).cmdline())
        finally:
            child.terminate();child.wait(timeout=5);log.close()
    print('Native Windows Python, psutil process identity, Win32 monitor enumeration and authenticated launcher HTTP passed')
if __name__=='__main__':main()
