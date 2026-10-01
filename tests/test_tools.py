import io
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from protocol import State, FRAME, RECORD_MAGIC, EVENT, event_packet, frames, unpack_state
from terrain import convert

class BridgeTests(unittest.TestCase):
    def test_bridge_runtime_is_scoped_to_session(self):
        from temporary_files import TemporaryFiles,stage_rl_runtime
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);runtime=root/'runtime';game=root/'game';session=root/'session'
            for path in (runtime,game,session):path.mkdir()
            (runtime/'msvcp140.dll').write_bytes(b'modern');(runtime/'vcruntime140_threads.dll').write_bytes(b'extra')
            (runtime/'unrelated.dll').write_bytes(b'ignore');(game/'msvcp140.dll').write_bytes(b'stock')
            temporary=TemporaryFiles(session);stage_rl_runtime(temporary,runtime,game)
            self.assertEqual((game/'msvcp140.dll').read_bytes(),b'modern')
            self.assertFalse((game/'unrelated.dll').exists())
            temporary.restore()
            self.assertEqual((game/'msvcp140.dll').read_bytes(),b'stock')
            self.assertFalse((game/'vcruntime140_threads.dll').exists())

    def test_cpp_python_wire_compatibility(self):
        binary = os.environ.get('BRIDGE_CORE_BINARY', str(ROOT/'build/bridge_core_tests'))
        with tempfile.TemporaryDirectory() as tmp:
            fixture = Path(tmp)/'state.bin'
            subprocess.run([binary,str(fixture)],check=True,stdout=subprocess.PIPE)
            data = fixture.read_bytes();p = unpack_state(data)
            self.assertEqual(len(data),176);self.assertEqual(p.session,7)
            self.assertEqual(p.car.position,(100,200,17));self.assertEqual(p.boost,80)
            self.assertEqual(p.pack(),data)
            event = EVENT.unpack(event_packet(p,3,velocity=(5,6,7),sequence=23))
            self.assertEqual(event[3],80);self.assertEqual(event[4:6],(23,7));self.assertEqual(event[7:9],(3,1))
            self.assertEqual(event[12:15],(5,6,7))

    def test_surface_feedback_wire_format(self):
        p=State()
        for kind in (4,5):
            e=EVENT.unpack(event_packet(p,kind,position=(100,200,30),velocity=(0,0,1)))
            self.assertEqual(e[7],kind)
            self.assertEqual(e[9:12],(100,200,30))
            self.assertEqual(e[12:15],(0,0,1))
        with self.assertRaises(ValueError):event_packet(p,15)

    def test_reject_malformed_packets_and_recordings(self):
        p = State()
        for data in (p.pack()[:-1],p.pack()+b'x', b'NOPE'+p.pack()[4:]):
            with self.assertRaises(ValueError):unpack_state(data)
        p.car.rotation=(0,0,0,0)
        with self.assertRaises(ValueError):unpack_state(p.pack())
        p = State();p.boost=float('nan')
        with self.assertRaises(ValueError):unpack_state(p.pack())
        for data in (b'not a recording',RECORD_MAGIC+b'x',RECORD_MAGIC+FRAME.pack(0,99999999)):
            with self.assertRaises(ValueError):list(frames(io.BytesIO(data)))
        data=State().pack()
        with self.assertRaises(ValueError):list(frames(io.BytesIO(RECORD_MAGIC+FRAME.pack(20,len(data))+data+FRAME.pack(10,len(data))+data)))

    def test_live_capture_demo_and_replay(self):
        # Bind an available loopback port; release only immediately before capture starts.
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as reserve:
            reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
        with tempfile.TemporaryDirectory() as tmp:
            file=Path(tmp)/'capture.rlsb'
            capture=subprocess.Popen([sys.executable,str(ROOT/'tools/bridge.py'),'capture','--port',str(port),
                '--seconds','0.6','--output',str(file)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            self.assertIn('Listening',capture.stdout.readline())
            demo=subprocess.run([sys.executable,str(ROOT/'tools/bridge.py'),'demo','--port',str(port),
                '--seconds','0.12','--hz','120'],capture_output=True,text=True,check=True)
            out,err=capture.communicate(timeout=3);self.assertEqual(capture.returncode,0,err)
            with file.open('rb') as stream:recorded=list(frames(stream))
            self.assertGreaterEqual(len(recorded),10)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as receiver:
                receiver.bind(('127.0.0.1',0));receiver.settimeout(2)
                replay=subprocess.Popen([sys.executable,str(ROOT/'tools/bridge.py'),'replay',str(file),
                    '--port',str(receiver.getsockname()[1]),'--speed','2'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                received=[unpack_state(receiver.recvfrom(1024)[0]) for _ in recorded]
                out,err=replay.communicate(timeout=3);self.assertEqual(replay.returncode,0,err)
            self.assertEqual([p.sequence for p in received],list(range(1,len(received)+1)))
            self.assertNotEqual(received[0].session,unpack_state(recorded[0][1]).session)
            self.assertEqual(received[-1].car.position,unpack_state(recorded[-1][1]).car.position)

    def test_terrain_winding_and_unit_conversion(self):
        with tempfile.TemporaryDirectory() as tmp:
            src,dst=Path(tmp)/'sky.obj',Path(tmp)/'rl.obj'
            src.write_text('v 10 20 30\nv 11 20 30\nv 10 21 30\nvn 0 0 1\nf 1//1 2//1 3//1\n')
            convert(src,dst,(10,20,30),(0,0,17),0.5,0)
            lines=dst.read_text().splitlines()
            self.assertIn('v 0 0 17',lines);self.assertIn('v 0 2 17',lines)
            self.assertIn('v 2 0 17',lines);self.assertIn('f 3//1 2//1 1//1',lines)

if __name__ == '__main__':
    unittest.main()
