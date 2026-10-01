import copy
import math
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from build_render_mesh import encode

class RenderMeshTests(unittest.TestCase):
    def scene(self):
        return {'meshes':[{'texture':'textures/rocketbridge/test.dds','vertices':[[0,0,0],[1,0,0],[0,1,0]],'normals':[[0,0,1]]*3,'uvs':[[0,0],[1,0],[0,1]],'colors':[[1,1,1,1]]*3,'triangles':[[2,0,1]]}]}
    def test_preserves_triangle_vertices_in_gpu_layout(self):
        data=encode(self.scene());magic,count,length=struct.unpack_from('<3I',data);self.assertEqual((magic,count),(0x314D4252,1))
        at=12+length;self.assertEqual(struct.unpack_from('<I',data,at)[0],3)
        v=struct.unpack_from('<12f',data,at+4);self.assertEqual(v[:3],(0,1,0));self.assertEqual(v[3:6],(0,0,1));self.assertEqual(v[6:8],(0,1));self.assertEqual(len(data),at+4+3*48)
    def test_rejects_bad_vertices_and_paths(self):
        for mutation in [lambda m:m['triangles'][0].__setitem__(0,3),lambda m:m['vertices'][0].__setitem__(0,math.nan),lambda m:m.__setitem__('texture','textures/rocketbridge/../../private.dds')]:
            scene=self.scene();mutation(scene['meshes'][0])
            with self.assertRaises(ValueError):encode(scene)
