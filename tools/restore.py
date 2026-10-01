#!/usr/bin/env python3
"""Restore bridge install/session backups, preserving subsequent user changes."""
import argparse
import hashlib
import json
from pathlib import Path

def restore(manifest,dry_run=False):
    entries=json.loads(manifest.read_text());restored=skipped=0
    for entry in reversed(entries):
        path=Path(entry['path'])
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest()!=entry['sha256']:
            print('Kept changed or absent file:',path);skipped+=1;continue
        if entry['backup'] and not Path(entry['backup']).is_file():raise RuntimeError('Backup missing: '+entry['backup'])
        print(('Would restore: ' if dry_run else 'Restore: ')+str(path))
        if not dry_run:
            if entry['backup']:path.write_bytes(Path(entry['backup']).read_bytes())
            else:path.unlink()
        restored+=1
    return restored,skipped

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('manifest',type=Path);p.add_argument('--dry-run',action='store_true');a=p.parse_args()
    restored,skipped=restore(a.manifest,a.dry_run)
    print(f'{restored} matched backups; {skipped} later changes preserved')

if __name__=='__main__':main()
