"""Recoverable session-scoped file overrides, shared by both launchers."""
import hashlib,json
from pathlib import Path

class TemporaryFiles:
    """Keep recovery bytes on disk; restore only files still equal to our write."""
    def __init__(self,folder):self.folder=folder;self.entries=[]
    def put(self,path,content):
        before=path.read_bytes() if path.exists() else None
        backup=self.folder/f'original-{len(self.entries)}.bin'
        if before is not None:backup.write_bytes(before)
        self.entries.append(dict(path=str(path),backup=str(backup) if before is not None else None,sha256=hashlib.sha256(content).hexdigest()))
        (self.folder/'temporary-files.json').write_text(json.dumps(self.entries,indent=2)+'\n')
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(content)
    def restore(self):
        for e in reversed(self.entries):
            path=Path(e['path'])
            if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest()!=e['sha256']:
                print('Kept a changed setting file; original saved in '+str(self.folder),flush=True);continue
            if e['backup']:path.write_bytes(Path(e['backup']).read_bytes())
            else:path.unlink()


def stage_rl_runtime(temporary,runtime_folder,game_folder):
    """Keep bridge CRT replacements inside the offline session, not normal RL."""
    for source in sorted(runtime_folder.glob("*.dll")):
        if source.name.lower().startswith(("msvcp140","vcruntime140")):
            temporary.put(game_folder/source.name,source.read_bytes())
