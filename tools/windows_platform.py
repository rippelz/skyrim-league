"""Native Windows paths/processes/window placement; no Proton or shell commands."""
import json, os, re, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def psutil_module():
    try:import psutil;return psutil
    except ImportError:raise RuntimeError('Install launcher dependencies: py -3 -m pip install -r requirements-windows.txt')

def script_command(pid,script):
    psutil=psutil_module()
    try:
        process=psutil.Process(pid);args=process.cmdline();cwd=Path(process.cwd())
        if any((Path(arg) if Path(arg).is_absolute() else cwd/arg).resolve()==ROOT/'tools'/script for arg in args[1:3]):return args
    except (psutil.Error,OSError,ValueError):pass
    return None

def steam_root():
    import winreg
    for hive,key,value in ((winreg.HKEY_CURRENT_USER,r'Software\Valve\Steam','SteamPath'),
                           (winreg.HKEY_LOCAL_MACHINE,r'SOFTWARE\WOW6432Node\Valve\Steam','InstallPath')):
        try:
            with winreg.OpenKey(hive,key) as handle:
                path=Path(winreg.QueryValueEx(handle,value)[0])
                if (path/'steam.exe').is_file():return path.resolve()
        except OSError:pass
    raise ValueError('Steam not found. Pass --steam with its installation folder.')

def libraries(steam):
    result=[Path(steam)]
    file=Path(steam)/'steamapps/libraryfolders.vdf'
    if file.exists():
        for value in re.findall(r'"path"\s+"((?:\\.|[^"\\])*)"',file.read_text(encoding='utf-8',errors='replace')):
            path=Path(value.replace('\\\\','\\'))
            if path not in result:result.append(path)
    return result

def find_game(steam,appid):
    for library in libraries(steam):
        manifest=library/'steamapps'/f'appmanifest_{appid}.acf'
        if not manifest.exists():continue
        text=manifest.read_text(encoding='utf-8',errors='replace')
        directory=re.search(r'"installdir"\s+"([^"\r\n]+)"',text)
        build=re.search(r'"buildid"\s+"(\d+)"',text)
        if directory:
            # A manifest supplies a folder name, never an absolute/traversal path.
            name=directory.group(1)
            if '/' in name or '\\' in name or name in ('.','..'):raise ValueError('Invalid Steam game directory')
            folder=library/'steamapps/common'/name
            if folder.is_dir():return folder.resolve(),build.group(1) if build else ''
    raise ValueError(f'Steam app {appid} not found in Steam libraries. Pass its folder explicitly.')

def bakkes_root():
    return Path(os.environ['APPDATA'])/'bakkesmod/bakkesmod'

def monitors():
    import ctypes
    from ctypes import wintypes as w
    class Info(ctypes.Structure):
        _fields_=[('size',w.DWORD),('rect',w.RECT),('work',w.RECT),('flags',w.DWORD),('device',w.WCHAR*32)]
    user=ctypes.WinDLL('user32',use_last_error=True)
    user.GetMonitorInfoW.argtypes=[w.HANDLE,ctypes.POINTER(Info)];user.GetMonitorInfoW.restype=w.BOOL
    callback=ctypes.WINFUNCTYPE(w.BOOL,w.HANDLE,w.HDC,ctypes.POINTER(w.RECT),w.LPARAM)
    user.EnumDisplayMonitors.argtypes=[w.HDC,ctypes.POINTER(w.RECT),callback,w.LPARAM]
    result=[]
    def collect(handle,dc,rect,data):
        info=Info();info.size=ctypes.sizeof(info)
        if user.GetMonitorInfoW(handle,ctypes.byref(info)):
            result.append(dict(name=info.device,description='Primary' if info.flags&1 else 'Display',
                bounds=[info.work.left,info.work.top,info.work.right,info.work.bottom]))
        return True
    user.EnumDisplayMonitors(None,None,callback(collect),0);return result

def place_window(pid,display='desktop',monitor=''):
    import ctypes
    from ctypes import wintypes as w
    user=ctypes.WinDLL('user32',use_last_error=True)
    callback=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    user.EnumWindows.argtypes=[callback,w.LPARAM]
    user.GetWindowThreadProcessId.argtypes=[w.HWND,ctypes.POINTER(w.DWORD)]
    user.IsWindowVisible.argtypes=[w.HWND];user.IsWindowVisible.restype=w.BOOL
    user.ShowWindow.argtypes=[w.HWND,ctypes.c_int]
    user.SetWindowPos.argtypes=[w.HWND,w.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,w.UINT]
    target=next((m for m in monitors() if m['name']==monitor),None) if monitor else None
    found=False
    def collect(hwnd,data):
        nonlocal found
        owner=w.DWORD();user.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        if owner.value==pid and user.IsWindowVisible(hwnd):
            if target:user.SetWindowPos(hwnd,None,target['bounds'][0]+40,target['bounds'][1]+40,0,0,0x0015) # NOSIZE|NOZORDER|NOACTIVATE
            if display=='minimized':user.ShowWindow(hwnd,7) # SW_SHOWMINNOACTIVE
            found=True
        return True
    user.EnumWindows(callback(collect),0);return found

def processes_at(executable):
    psutil=psutil_module();target=os.path.normcase(str(Path(executable).resolve()));result=[]
    for process in psutil.process_iter(['pid','exe','create_time']):
        try:
            if process.info['exe'] and os.path.normcase(str(Path(process.info['exe']).resolve()))==target:result.append(process)
        except (psutil.Error,OSError):pass
    return result

def identity(process):return dict(pid=process.pid,created=process.create_time(),exe=process.exe())

def matching_process(record):
    psutil=psutil_module()
    try:
        process=psutil.Process(record['pid'])
        if abs(process.create_time()-record['created'])<.001 and os.path.normcase(process.exe())==os.path.normcase(record['exe']):return process
    except (psutil.Error,OSError,KeyError):pass
    return None
