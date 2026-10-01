#!/usr/bin/env python3
"""Loopback-only bridge launcher. Starts/stops only this project's sessions."""
import argparse, configparser, json, os, secrets, signal, subprocess, sys, threading, time, shutil
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit
ROOT=Path(__file__).resolve().parents[1]

def open_browser(url):
 if os.name=="nt":
  import webbrowser;webbrowser.open(url)
 else:subprocess.Popen(["xdg-open",url],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)

def manager_running(pid):
 if os.name=="nt":
  from windows_platform import script_command
  return bool(script_command(pid,"manager.py"))
 try:
  proc=Path("/proc")/str(pid);parts=[v.decode(errors="replace") for v in (proc/"cmdline").read_bytes().split(b"\0") if v];cwd=(proc/"cwd").resolve()
  return any((Path(v) if Path(v).is_absolute() else cwd/v).resolve()==ROOT/"tools/manager.py" for v in parts[1:3])
 except (OSError,ValueError):return False

FIELDS={
 'CastShadows':(0,1,int,1),
 'RenderBrightness':(.02,3,float,1),
 'NpcDamageScale':(.0,.1,float,.03), 'NpcCooldownMs':(100,10000,int,750),
 'InterpolationMs':(0,200,int,30), 'TimeoutMs':(100,5000,int,3000),
 'CarScale':(.01,10,float,1), 'BallScale':(.01,10,float,1),
 'FollowCamera':(0,1,int,1), 'SuppressSurvivalPrompt':(0,1,int,1), 'AutoStart':(0,1,int,0)}

def command(pid):
 try:
  args=[a.decode(errors='replace') for a in (Path('/proc')/str(pid)/'cmdline').read_bytes().split(b'\0') if a]
  cwd=(Path('/proc')/str(pid)/'cwd').resolve()
  for arg in args[1:3]:
   if (Path(arg) if Path(arg).is_absolute() else cwd/arg).resolve()==ROOT/'tools/launch.py':return args
 except (OSError,ValueError):pass
 return None

class Manager:
 def __init__(self):
  self.child=None;self.lock=threading.Lock();self.stopping=set()
  state=json.loads((ROOT/'build/install-state.json').read_text())
  self.ini=Path(state['skyrim'])/'Data/SKSE/Plugins/SkyrimRocketBridge.ini'
  self.log=ROOT/'build/manager-launch.log'
 def instances(self):
  result=[];known=set()
  for folder in sorted((ROOT/'build/sessions').glob('*'),reverse=True):
   try:
    data=json.loads((folder/'session.json').read_text());pid=int(data['pid']);args=command(pid)
    active=bool(args);known.add(pid)
    if active or len(result)<8:
     result.append(dict(id=folder.name,pid=pid,active=active,stopping=active and pid in self.stopping,
      layout=data.get('layout'),desktop=bool(args and '--desktop-skyrim' in args),panel=active and bool(data.get('panels')) and (folder/'panel.html').exists()))
   except (OSError,ValueError,KeyError):pass
  # Include initialization before launch.py has written its session.json.
  for proc in Path('/proc').iterdir():
   if proc.name.isdecimal() and int(proc.name) not in known:
    args=command(int(proc.name))
    if args:result.insert(0,dict(id='Starting',pid=int(proc.name),active=True,stopping=int(proc.name) in self.stopping,desktop='--desktop-skyrim' in args,panel=False))
  if self.child:self.child.poll()
  return result
 def settings(self):
  cfg=configparser.ConfigParser(interpolation=None);cfg.read(self.ini)
  return {key:cast(cfg.get('Bridge',key,fallback=str(default))) for key,(_,_,cast,default) in FIELDS.items()}
 def monitors(self):
  if not shutil.which("hyprctl"):return []
  try:return [{"name":m["name"],"description":m.get("model",m["name"])} for m in json.loads(subprocess.check_output(["hyprctl","monitors","-j"],stderr=subprocess.DEVNULL))]
  except (OSError,ValueError,subprocess.CalledProcessError):return []
 def status(self):
  instances=self.instances()
  try:log=self.log.read_text(errors='replace')[-5000:]
  except OSError:log=''
  return dict(capabilities=dict(displays=['private','desktop'],workspaces=True,platform='linux'),monitors=self.monitors(),instances=instances,settings=self.settings(),busy=any(x['active'] for x in instances),log=log)
 def start(self,data):
  if any(x['active'] for x in self.instances()):raise ValueError('Stop the current bridge session first.')
  # Existing Steam games must remain under the normal launcher's ownership checks.
  args=[sys.executable,str(ROOT/'tools/launch.py'),'--retry-skyrim']
  choices=data.get('games')
  if not isinstance(choices,dict) or set(choices)!= {'rl','skyrim'}:raise ValueError('Choose settings for both games')
  for game,choice in choices.items():
   if not isinstance(choice,dict):raise ValueError('Invalid game settings')
   enabled=choice.get('enabled');display=choice.get('display');workspace=choice.get('workspace');monitor=choice.get('monitor','')
   if not isinstance(enabled,bool) or display not in ('desktop','private'):raise ValueError('Invalid game display')
   if not enabled:args.append('--no-'+game);continue
   if display=='desktop':
    if not isinstance(workspace,int) or isinstance(workspace,bool) or not 1<=workspace<=100:raise ValueError('Workspace must be 1–100')
    if not isinstance(monitor,str) or (monitor and monitor not in [m['name'] for m in self.monitors()]):raise ValueError('Choose a connected monitor')
    args.extend(['--desktop-'+game,'--'+game+'-workspace',str(workspace)])
    if monitor:args.extend(['--'+game+'-monitor',monitor])
  if not any(c['enabled'] for c in choices.values()):raise ValueError('Enable at least one game')
  if not self.settings()['AutoStart']:args.append('--manual-bridge')
  self.log.parent.mkdir(parents=True,exist_ok=True)
  with self.log.open('w') as log:self.child=subprocess.Popen(args,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
  return dict(pid=self.child.pid)
 def stop(self,data):
  pid=int(data.get('pid',0))
  if not command(pid):raise ValueError('That session has already stopped.')
  os.kill(pid,signal.SIGINT);self.stopping.add(pid)
  return dict(stopping=pid)
 def save(self,data):
  if any(x['active'] for x in self.instances()):raise ValueError('Stop the bridge before changing settings.')
  if not isinstance(data,dict) or set(data)!=set(FIELDS):raise ValueError('Invalid settings')
  values={}
  for key,value in data.items():
   lo,hi,cast,_=FIELDS[key]
   if isinstance(value,bool):value=int(value)
   number=float(value)
   if not lo<=number<=hi or (cast is int and not number.is_integer()):raise ValueError('Invalid '+key)
   values[key]=str(cast(number))
  # Preserve comments, model paths and all settings outside the UI.
  import re
  text=self.ini.read_text();before=text
  for key,value in values.items():
   expression=r'(?mi)^'+re.escape(key)+r'\s*=.*$'
   if re.search(expression,text):text=re.sub(expression,key+'='+value,text)
   else:text+='\n'+key+'='+value+'\n'
  if text!=before:
   backup=ROOT/'build/ui-settings-backups'/str(time.time_ns());backup.mkdir(parents=True)
   (backup/self.ini.name).write_text(before)
   temporary=self.ini.with_suffix('.ini.ui-next');temporary.write_text(text);temporary.replace(self.ini)
  return dict(saved=True)

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--port',type=int,default=8767);parser.add_argument('--open',action='store_true',help='Open the launcher in your browser');args=parser.parse_args()
 record=ROOT/'build/manager.json'
 if record.exists():
  try:
   previous=json.loads(record.read_text())
   if manager_running(previous['pid']):
    print('Bridge launcher UI: '+previous['url'],flush=True)
    if args.open:open_browser(previous['url'])
    return
  except (OSError,ValueError,KeyError):pass
 if os.name=="nt":
  from windows_manager import WindowsManager
  manager=WindowsManager()
 else:manager=Manager()
 token=os.environ.get("SKYRIM_LEAGUE_UI_TOKEN") or secrets.token_urlsafe(32)
 class Handler(BaseHTTPRequestHandler):
  def log_message(self,*_):pass
  def reply(self,status,body,kind='application/json'):
   raw=body.encode() if isinstance(body,str) else json.dumps(body).encode()
   self.send_response(status);self.send_header('Content-Type',kind+'; charset=utf-8');self.send_header('Content-Length',str(len(raw)))
   self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.send_header('Referrer-Policy','no-referrer');self.end_headers();self.wfile.write(raw)
  def authorized(self):return secrets.compare_digest(self.headers.get('X-Bridge-Token',''),token)
  def do_GET(self):
   path=urlsplit(self.path).path
   if self.headers.get('Host') not in (f'127.0.0.1:{args.port}',f'localhost:{args.port}'):return self.reply(403,{'error':'Invalid host'})
   if path=='/':return self.reply(200,(ROOT/'tools/manager.html').read_text(),'text/html')
   if not self.authorized():return self.reply(403,{'error':'Open the launcher using its private link.'})
   with manager.lock:
    if path=='/api/status':return self.reply(200,manager.status())
    if path.startswith('/api/panel/'):
     ident=path.removeprefix('/api/panel/')
     if any(x['id']==ident and x['active'] and x['panel'] for x in manager.instances()):return self.reply(200,{'html':(ROOT/'build/sessions'/ident/'panel.html').read_text()})
   self.reply(404,{'error':'Not found'})
  def do_POST(self):
   if not self.authorized() or self.headers.get('Origin') not in (f'http://127.0.0.1:{args.port}',f'http://localhost:{args.port}'):return self.reply(403,{'error':'Invalid launcher request'})
   try:
    length=int(self.headers.get('Content-Length','0'))
    if not 0<length<=8192:raise ValueError('Invalid request size')
    data=json.loads(self.rfile.read(length));path=urlsplit(self.path).path
    if not isinstance(data,dict):raise ValueError('Expected an object')
    with manager.lock:
     if path=='/api/start':result=manager.start(data)
     elif path=='/api/stop':result=manager.stop(data)
     elif path=='/api/settings':result=manager.save(data)
     else:return self.reply(404,{'error':'Not found'})
    self.reply(200,result)
   except (ValueError,OSError,TypeError) as e:self.reply(400,{'error':str(e)})
 server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
 url=f'http://127.0.0.1:{args.port}/#{token}'
 record=ROOT/'build/manager.json';record.parent.mkdir(exist_ok=True)
 fd=os.open(record,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
 with os.fdopen(fd,'w') as out:json.dump(dict(pid=os.getpid(),url=url),out)
 print('Bridge launcher UI: '+url,flush=True)
 if args.open:open_browser(url)
 try:server.serve_forever()
 except KeyboardInterrupt:pass
 finally:server.server_close();record.unlink(missing_ok=True)
if __name__=='__main__':main()
