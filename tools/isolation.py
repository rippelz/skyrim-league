"""Manage a task-owned X server and processes without affecting the working desktop."""
import os
from pathlib import Path
import re
import secrets
import select
import signal
import subprocess
import tempfile
import time
import uuid

def require_steam_client(steam, proc_root=Path('/proc')):
    """Do not let a game's restart request bootstrap Steam on our private X server."""
    executable=(steam/'ubuntu12_32/steam').resolve()
    for proc in proc_root.iterdir():
        if not proc.name.isdecimal():continue
        try:
            if (proc/'exe').resolve()!=executable:continue
            environment=(proc/'environ').read_bytes().split(b'\0')
            if not any(value.startswith(b'ROCKET_SKYRIM_ISOLATED=') for value in environment):return
        except OSError:pass
    raise RuntimeError('Open Steam normally and sign in before launching the bridge. No game or private Steam instance was started.')

def configured_proton(steam, appid):
    """Use Steam's existing per-game compatibility choice."""
    config=(steam/'config/config.vdf').read_text(errors='replace')
    mapping=config.partition('"CompatToolMapping"')[2]
    match=re.search(r'"'+str(appid)+r'"\s*\{\s*"name"\s*"([^\"]+)"',mapping)
    if match:
        name=match.group(1)
        for base in (steam/'compatibilitytools.d',Path('/usr/share/steam/compatibilitytools.d')):
            proton=base/name/'proton'
            if proton.is_file():return proton
        for manifest in (steam/'steamapps/common').glob('*/compatibilitytool.vdf'):
            if '"'+name+'"' in manifest.read_text(errors='replace'):
                proton=manifest.parent/'proton'
                if proton.is_file():return proton
    proton=steam/'steamapps/common/Proton - Experimental/proton'
    if not proton.is_file():raise RuntimeError('Configured Proton installation not found')
    return proton

def graphics_environment(env):
    # The Steam Fossilize layer crashes this offline RL build under Xvfb.
    # Scope overrides to the launched children; leave desktop settings alone.
    # Xvfb lacks DRI3. Mesa's copy-based WSI can present GPU-rendered Vulkan
    # frames there without switching the game to a software renderer.
    env.update(MESA_VK_WSI_DEBUG='sw',DISABLE_VK_LAYER_VALVE_steam_fossilize_1='1',DISABLE_LSFGVK='1',
               VK_LOADER_LAYERS_DISABLE='VK_LAYER_VALVE_steam_fossilize*,VK_LAYER_LSFGVK_frame_generation')
    return env

def proton_command(steam, appid, *arguments, in_prefix=False):
    proton=configured_proton(steam,appid)
    manifest=proton.parent/'toolmanifest.vdf'
    required=re.search(r'"require_tool_appid"\s*"(\d+)"',manifest.read_text()) if manifest.exists() else None
    runtime_ids={'4183110':'SteamLinuxRuntime_4','1628350':'SteamLinuxRuntime_sniper','1391110':'SteamLinuxRuntime_soldier'}
    if required and required.group(1) in runtime_ids:
        entry=steam/'steamapps/common'/runtime_ids[required.group(1)]/'_v2-entry-point'
        if not entry.is_file():raise RuntimeError('Configured Steam Linux Runtime is not installed')
        return [str(entry),'--verb=run','--',str(proton),'runinprefix' if in_prefix else 'run',*map(str,arguments)]
    return [str(proton),'runinprefix' if in_prefix else 'run',*map(str,arguments)]

class DesktopDisplay:
    """Launch on the caller's desktop; stop only children bearing our marker."""
    def __init__(self):
        if not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
            raise RuntimeError('Run --desktop-skyrim from a terminal on your desktop.')
        self.marker=uuid.uuid4().hex
    def environment(self):
        env=os.environ.copy()
        env.update(ROCKET_SKYRIM_ISOLATED=self.marker,SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='1')
        return env
    def close(self):
        marker=('ROCKET_SKYRIM_ISOLATED='+self.marker).encode()
        def owned():
            result=[]
            for proc in Path('/proc').iterdir():
                if not proc.name.isdecimal():continue
                try:
                    if marker in (proc/'environ').read_bytes().split(b'\0'):result.append(int(proc.name))
                except OSError:pass
            return result
        for pid in owned():
            try:os.kill(pid,signal.SIGTERM)
            except OSError:pass
        deadline=time.monotonic()+.5
        while time.monotonic()<deadline and owned():time.sleep(.05)
        for pid in owned():
            try:os.kill(pid,signal.SIGKILL)
            except OSError:pass

class VirtualDisplay:
    def __init__(self, log, size='1280x720x24'):
        self.marker=uuid.uuid4().hex
        self.auth_folder=tempfile.TemporaryDirectory(prefix='rocketbridge-xauth-')
        self.auth=Path(self.auth_folder.name)/'authority'
        self.auth.touch(mode=0o600)
        number=next(n for n in range(90,190) if not Path(f'/tmp/.X11-unix/X{n}').exists() and not Path(f'/tmp/.X{n}-lock').exists())
        requested=':'+str(number)
        subprocess.run(['xauth','-f',str(self.auth),'add',requested,'.',secrets.token_hex(16)],check=True,stdout=log,stderr=subprocess.STDOUT)
        read_fd,write_fd=os.pipe()
        self.server=subprocess.Popen(['Xvfb',requested,'-displayfd',str(write_fd),'-screen','0',size,'-nolisten','tcp','-auth',str(self.auth),'-noreset'],
                                     pass_fds=(write_fd,),stdout=log,stderr=subprocess.STDOUT)
        os.close(write_fd)
        if not select.select([read_fd],[],[],5)[0]:
            self.server.terminate();raise RuntimeError('Virtual display failed to start')
        self.display=':'+os.read(read_fd,64).decode().strip()
        # Xvfb may report readiness again after its startup reset. Keep the
        # reader alive until shutdown so its displayfd write cannot hit EPIPE.
        self.display_fd=read_fd
        if self.display!=requested:
            self.server.terminate();self.auth_folder.cleanup();raise RuntimeError('Xvfb selected an unexpected display')
        root=Path(__file__).resolve().parents[1]
        manager=root/'.deps/viewer/usr/bin/openbox'
        if manager.is_file():
            env=self.environment()
            env['LD_LIBRARY_PATH']=str(root/'.deps/viewer/usr/lib')
            env['XDG_DATA_DIRS']=str(root/'.deps/viewer/usr/share')+':/usr/local/share:/usr/share'
            self.manager=subprocess.Popen([str(manager),'--sm-disable','--config-file',str(root/'tools/private-openbox.xml')],env=env,stdout=log,stderr=subprocess.STDOUT)
    def environment(self):
        env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None)
        env['XAUTHORITY']=str(self.auth)
        env.update(DISPLAY=self.display,ROCKET_SKYRIM_ISOLATED=self.marker,SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='1')
        return env
    def close(self):
        marker=('ROCKET_SKYRIM_ISOLATED='+self.marker).encode()
        def owned():
            result=[]
            for proc in Path('/proc').iterdir():
                if not proc.name.isdecimal():continue
                try:
                    if marker in (proc/'environ').read_bytes().split(b'\0'):result.append(int(proc.name))
                except OSError:pass
            return result
        for pid in owned():
            try:os.kill(pid,signal.SIGTERM)
            except OSError:pass
        self.server.terminate()
        try:self.server.wait(timeout=5)
        except subprocess.TimeoutExpired:self.server.kill()
        deadline=time.monotonic()+.5
        while time.monotonic()<deadline and owned():time.sleep(.05)
        # Wine's system-device helpers may ignore SIGTERM. Recheck the unique
        # marker before escalation; no prefix-wide or name-based kill is used.
        for pid in owned():
            try:os.kill(pid,signal.SIGKILL)
            except OSError:pass
        if hasattr(self,'manager'):
            try:self.manager.wait(timeout=2)
            except subprocess.TimeoutExpired:self.manager.kill();self.manager.wait()
        os.close(self.display_fd)
        self.auth_folder.cleanup()
