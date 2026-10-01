#!/usr/bin/env python3
"""Prepare a local browser viewer without installing system packages (Arch/CachyOS)."""
import hashlib
from pathlib import Path
import subprocess
import urllib.request
import venv
from prepare_viewer import prepare

ROOT=Path(__file__).resolve().parents[1]

def main():
    base=ROOT/'.deps/viewer';base.mkdir(parents=True,exist_ok=True)
    keyring=base/'archlinux-package-keys.gpg'
    subprocess.run(['gpg','--batch','--yes','--dearmor','--output',str(keyring),'/usr/share/pacman/keyrings/archlinux.gpg'],check=True)
    # Pacman's repository metadata supplies both URL and SHA256.
    listing=subprocess.check_output(['pacman','-Sp','--print-format','%l %h','extra/x11vnc','extra/libvncserver','extra/openbox'],text=True)
    for line in listing.splitlines():
        url,digest=line.rsplit(' ',1)
        package=base/url.rsplit('/',1)[-1]
        if not package.exists() or hashlib.sha256(package.read_bytes()).hexdigest()!=digest:
            data=urllib.request.urlopen(url,timeout=30).read()
            if hashlib.sha256(data).hexdigest()!=digest:raise SystemExit('Package checksum mismatch')
            package.write_bytes(data)
        signature=package.with_name(package.name+'.sig')
        if not signature.exists():signature.write_bytes(urllib.request.urlopen(url+'.sig',timeout=30).read())
        subprocess.run(['gpgv','--keyring',str(keyring),str(signature),str(package)],check=True)
        listing=subprocess.check_output(['bsdtar','-tf',str(package)],text=True).splitlines()
        if any(n.startswith('/') or '..' in Path(n).parts for n in listing):raise SystemExit('Unsafe package path')
        subprocess.run(['bsdtar','-xf',str(package),'-C',str(base)],check=True)
    client=ROOT/'.deps/noVNC'
    if not client.exists():
        subprocess.run(['git','clone','--depth','1','--branch','v1.7.0','https://github.com/novnc/noVNC.git',str(client)],check=True)
    if subprocess.check_output(['git','-C',str(client),'describe','--tags','--exact-match'],text=True).strip()!='v1.7.0':raise SystemExit('Unexpected noVNC version')
    prepare(client)
    runtime=ROOT/'.deps/viewer-env'
    if not (runtime/'bin/python').exists():venv.create(runtime,with_pip=True)
    subprocess.run([str(runtime/'bin/python'),'-m','pip','install','websockify==0.13.0'],check=True)
    print('Viewer dependencies ready; no desktop windows opened.')

if __name__=='__main__':main()
