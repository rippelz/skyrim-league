#!/usr/bin/env python3
"""Fetch pinned public SDK sources and the checksum-verified CommonLib bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEPS = {
    'bullet-2.82': ('https://github.com/bulletphysics/bullet3.git','19f999ac087e68ffc2551ffb73e35e60271a0d27'),
    'minhook': ('https://github.com/TsudaKageyu/minhook.git','8af6b4acae5a9388fd742b56fa79ece89d96f823'),
    'BakkesModSDK': ('https://github.com/bakkesmodorg/BakkesModSDK.git','479e8f571cf554b25f4eeb64d611dec4133edcaf'),
    'CommonLibSSE-NG-current': ('https://github.com/alandtse/CommonLibSSE-NG.git','2dde70e8bdf9890bbd5e648966c7d2c24e83092f'),
    'spdlog-current': ('https://github.com/gabime/spdlog.git','486b55554f11c9cccc913e11a87085b2a91f706f'),
    'fmt': ('https://github.com/fmtlib/fmt.git','407c905e45ad75fc29bf0f9bb7c5c2fd3475976f'),
    'rapidcsv': ('https://github.com/d99kris/rapidcsv.git','d76cf1c08e79be1eb04c9d4d8eb7893221127991'),
}

def run(*args):
    return subprocess.check_output(list(args), text=True).strip()

def main():
    p = argparse.ArgumentParser(description=__doc__);p.add_argument('--source-only', action='store_true');a = p.parse_args()
    base = ROOT/'.deps';base.mkdir(exist_ok=True)
    for name, (url, commit) in DEPS.items():
        folder = base/name
        if not folder.exists():
            run('git','clone','--filter=blob:none','--no-checkout',url,str(folder))
            run('git','-C',str(folder),'checkout',commit)
        actual = run('git','-C',str(folder),'rev-parse','HEAD')
        if actual != commit:
            raise SystemExit(f'{folder} has a different commit; preserve it and set SDK paths explicitly')
        print(name, actual)
    if a.source_only:
        return
    name = 'commonlibsse-ng-prebuilt-v7.1.0-all-msvc-cmake.zip'
    stem = 'https://github.com/alandtse/CommonLibSSE-NG/releases/download/v7.1.0/'
    expected = urllib.request.urlopen(stem+name+'.sha256', timeout=30).read().decode().split()[0]
    archive = base/name
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        temporary = archive.with_suffix('.partial')
        urllib.request.urlretrieve(stem+name, temporary)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
            raise SystemExit('CommonLib checksum mismatch')
        temporary.replace(archive)
    bundle = base/'commonlib-prebuilt'
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            if not (bundle/entry.filename).resolve().is_relative_to(bundle.resolve()):
                raise SystemExit('Unsafe archive path')
        z.extractall(bundle)
    (base/'dependency-lock.json').write_text(json.dumps({'sources':DEPS,'commonlib_bundle_sha256':expected}, indent=2)+'\n')
    print('Verified CommonLib prebuilt:', expected)

if __name__ == '__main__':
    main()
