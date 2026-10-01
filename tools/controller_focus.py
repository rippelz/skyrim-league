#!/usr/bin/env python3
"""Publish native Skyrim focus without activating or moving any window."""
import argparse,json,os,subprocess,time
from pathlib import Path

def main():
 p=argparse.ArgumentParser();p.add_argument('--launcher-pid',type=int,required=True);p.add_argument('--private-skyrim',action='store_true');a=p.parse_args()
 root=Path(__file__).resolve().parents[1];state=json.loads((root/'build/install-state.json').read_text())
 target=Path(state['bakkesmod'])/'data/rocket-skyrim-focus.txt';target.parent.mkdir(parents=True,exist_ok=True)
 while Path(f'/proc/{a.launcher_pid}').exists():
  focused=a.private_skyrim
  if not focused:
   try:
    window=json.loads(subprocess.check_output(['hyprctl','activewindow','-j'],timeout=1))
    focused=window.get('class')=='steam_app_489830' and window.get('title')=='Skyrim Special Edition'
   except (OSError,ValueError,subprocess.SubprocessError):focused=False
  staged=target.with_suffix('.next');staged.write_text('1' if focused else '0');os.replace(staged,target);time.sleep(.25)
 target.unlink(missing_ok=True)
if __name__=='__main__':main()
