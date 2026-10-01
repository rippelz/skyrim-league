#!/usr/bin/env python3
"""Package native Windows bridge binaries, sources and launchers; excludes game assets/settings."""
import argparse,hashlib,json,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,default=ROOT/'dist/Skyrim-League-Windows.zip');a=p.parse_args()
    files=[Path(n) for n in ('README.md','WINDOWS.md','LICENSE','THIRD_PARTY.md','CMakeLists.txt','requirements-windows.txt','bridge-ui.cmd','launch-bridge.cmd','setup-windows.cmd')]
    for folder in ('tools','shared','tests','bakkes-plugin','skse-plugin','licenses','docs'):
        files.extend(f.relative_to(ROOT) for f in (ROOT/folder).rglob('*') if f.is_file() and f.suffix in ('.py','.cpp','.hpp','.h','.html','.ini','.txt','.cmake','.md','.svg','.xml','.png'))
    files.extend(Path(n) for n in ('build-win/bakkes-plugin/RocketSkyrim.dll','build-win/skse-plugin/SkyrimRocketBridge.dll','build-win/bridge_timing_tests.exe','build-win/bridge_contact_tests.exe'))
    manifest={}
    for relative in files:
        if not (ROOT/relative).is_file():raise SystemExit('Missing package input: '+str(relative))
        manifest[relative.as_posix()]=hashlib.sha256((ROOT/relative).read_bytes()).hexdigest()
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(a.output,'w',zipfile.ZIP_DEFLATED) as archive:
        for relative in sorted(set(files)):archive.write(ROOT/relative,relative.as_posix())
        archive.writestr('package-checksums.json',json.dumps(manifest,indent=2)+'\n')
    print('Windows package:',a.output)
if __name__=='__main__':main()
