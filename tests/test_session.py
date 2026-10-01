import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from launch import TemporaryFiles
from restore import restore
from isolation import DesktopDisplay, VirtualDisplay, require_steam_client

class SessionTests(unittest.TestCase):
    def test_desktop_cleanup_preserves_unrelated_process(self):
        with patch.dict(os.environ,{'DISPLAY':':desktop-test','WAYLAND_DISPLAY':'wayland-test'}):
            display=DesktopDisplay()
            self.assertEqual(display.environment()['DISPLAY'],':desktop-test')
            self.assertEqual(display.environment()['WAYLAND_DISPLAY'],'wayland-test')
            owned=subprocess.Popen([sys.executable,'-c','import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); print("ready",flush=True); time.sleep(60)'],env=display.environment(),stdout=subprocess.PIPE,text=True)
            other=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'])
            try:
                self.assertEqual(owned.stdout.readline().strip(),'ready')
                display.close()
                self.assertIsNotNone(owned.wait(timeout=2))
                self.assertIsNone(other.poll())
            finally:
                if owned.poll() is None:owned.kill();owned.wait()
                owned.stdout.close()
                other.terminate();other.wait()

    def test_steam_preflight_requires_existing_nonisolated_client(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);steam=root/'steam';binary=steam/'ubuntu12_32/steam'
            binary.parent.mkdir(parents=True);binary.touch()
            processes=root/'proc';processes.mkdir();child=processes/'123';child.mkdir()
            (child/'exe').symlink_to(binary)
            (child/'environ').write_bytes(b'ROCKET_SKYRIM_ISOLATED=private\0')
            with self.assertRaises(RuntimeError):require_steam_client(steam,processes)
            (child/'environ').write_bytes(b'DISPLAY=:0\0')
            require_steam_client(steam,processes)
            (child/'exe').unlink()
            with self.assertRaises(RuntimeError):require_steam_client(steam,processes)

    @unittest.skipUnless(__import__('shutil').which('Xvfb') and __import__('shutil').which('xauth'),'Xvfb/xauth not installed')
    def test_cleanup_stops_owned_stubborn_child_and_preserves_other_process(self):
        sockets={p:(p.lstat().st_ino,p.readlink() if p.is_symlink() else None) for p in Path('/tmp/.X11-unix').glob('*')}
        with tempfile.TemporaryFile(mode='w+') as log:
            display=VirtualDisplay(log);owned=other=None
            try:
                owned=subprocess.Popen([sys.executable,'-c','import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); print("ready",flush=True); time.sleep(60)'],env=display.environment(),stdout=subprocess.PIPE,text=True)
                self.assertEqual(owned.stdout.readline().strip(),'ready')
                other=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'])
                display.close();self.assertIsNotNone(owned.wait(timeout=2));self.assertIsNone(other.poll())
                for path,state in sockets.items():
                    self.assertEqual((path.lstat().st_ino,path.readlink() if path.is_symlink() else None),state)
            finally:
                if display.server.poll() is None:display.close()
                if owned and owned.poll() is None:owned.kill();owned.wait()
                if owned and owned.stdout:owned.stdout.close()
                if other and other.poll() is None:other.terminate();other.wait()

    def test_recovery_preserves_later_user_edits(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);file=root/'settings.ini';file.write_bytes(b'original')
            changes=TemporaryFiles(root);changes.put(file,b'bridge setting');changes.restore()
            self.assertEqual(file.read_bytes(),b'original')
            changes=TemporaryFiles(root);changes.put(file,b'bridge setting');file.write_bytes(b'user edit')
            changes.restore();self.assertEqual(file.read_bytes(),b'user edit')
            count,skipped=restore(root/'temporary-files.json');self.assertEqual((count,skipped),(0,1))

    @unittest.skipUnless((ROOT/'.deps/viewer/usr/bin/x11vnc').exists() and (ROOT/'build/install-state.json').exists(),'local viewer dependencies not installed')
    def test_viewer_authentication_and_session_cleanup(self):
        with tempfile.TemporaryFile(mode='w+') as log:
            process=subprocess.Popen([sys.executable,str(ROOT/'tools/launch.py'),'--viewer-only','--seconds','5'],stdout=log,stderr=subprocess.STDOUT)
            try:
                deadline=time.monotonic()+4
                for port in (6081,6082):
                    while True:
                        try:
                            with urllib.request.urlopen(f'http://127.0.0.1:{port}/vnc.html',timeout=.3) as page:self.assertIn(b'noVNC',page.read())
                            break
                        except OSError:
                            if time.monotonic()>deadline:raise
                            time.sleep(.05)
                for port in (5901,5902):
                    while True:
                        try:peer=socket.create_connection(('127.0.0.1',port),timeout=1);break
                        except OSError:
                            if time.monotonic()>deadline:raise
                            time.sleep(.05)
                    with peer:
                        greeting=peer.recv(12);self.assertTrue(greeting.startswith(b'RFB '));peer.sendall(b'RFB 003.008\n')
                        count=peer.recv(1)[0];types=peer.recv(count)
                        self.assertIn(2,types);self.assertNotIn(1,types) # require the generated VNC password
                self.assertEqual(process.wait(timeout=8),0)
                for port in (6081,6082,5901,5902):
                    with socket.socket() as probe:
                        # Closed TCP peers may leave TIME_WAIT; an active
                        # listener still prevents this exclusive bind.
                        probe.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
                        probe.bind(('127.0.0.1',port))
            finally:
                if process.poll() is None:process.terminate();process.wait(timeout=8)
                log.seek(0)
                if process.returncode:print(log.read())

if __name__=='__main__':unittest.main()
