#!/usr/bin/env python3
"""Copy this user's installed visual assets for private transfer to another PC."""
import argparse,json,shutil
from pathlib import Path
from install_windows import asset_files
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('destination',type=Path);p.add_argument('--data',type=Path);a=p.parse_args()
    data=a.data or Path(json.loads((ROOT/'build/install-state.json').read_text())['skyrim'])/'Data'
    destination=a.destination.resolve()
    if destination.exists():p.error('Choose a new destination folder to preserve existing files.')
    files=asset_files(data)
    for source in files:
        target=destination/source.relative_to(data);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
    print('Private asset folder:',destination,'('+str(len(files))+' files). Keep these locally extracted game assets private.')
if __name__=='__main__':main()
