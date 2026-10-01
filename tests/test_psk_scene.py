import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from psk_to_scene import convert

class SceneNormalsTests(unittest.TestCase):
 def test_unreal_convex_winding_becomes_outward_target_normals(self):
  points=[(1,1,1),(-1,-1,1),(-1,1,-1),(1,-1,-1)]
  faces=[]
  for ids in [(0,1,2),(0,3,1),(0,2,3),(1,3,2)]:
   a,b,c=[points[i] for i in ids];u=[b[i]-a[i] for i in range(3)];v=[c[i]-a[i] for i in range(3)]
   n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
   center=[sum(points[j][i] for j in ids)/3 for i in range(3)]
   if sum(n[i]*center[i] for i in range(3))>0:ids=(ids[2],ids[1],ids[0])
   faces.append(ids)
  def chunk(name,size,records):
   return struct.pack('<20s3i',name.encode(),0,size,len(records))+b''.join(records)
  data=chunk('ACTRHEAD',0,[])+chunk('PNTS0000',12,[struct.pack('<3f',*p) for p in points])
  data+=chunk('VTXW0000',16,[struct.pack('<I2fBBH',i,.25,.5,0,0,0) for i in range(4)])
  data+=chunk('MATT0000',88,[b'Test'+bytes(84)])
  data+=chunk('FACE0000',12,[struct.pack('<3HBBI',*ids,0,0,0) for ids in faces])
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp)/'convex.psk';p.write_bytes(data);mesh=convert(p,{})['meshes'][0]
  for point,normal in zip(mesh['vertices'],mesh['normals']):
   self.assertGreater(sum(point[i]*normal[i] for i in range(3)),0)
   self.assertAlmostEqual(sum(v*v for v in normal),1)
if __name__=='__main__':unittest.main()
