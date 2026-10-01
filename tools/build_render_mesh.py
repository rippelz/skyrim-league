#!/usr/bin/env python3
"""Build private direct-render vertex data from the extracted scene."""
import json
import math
from pathlib import Path
import struct

def encode(scene):
    meshes=scene['meshes']
    if len(meshes)>32: raise ValueError('Too many materials')
    output=bytearray(struct.pack('<II',0x314D4252,len(meshes)))
    for mesh in meshes:
        texture=mesh['texture'].replace('\\','/').encode()
        if len(texture)>256 or not texture.startswith(b'textures/rocketbridge/') or b'..' in texture: raise ValueError('Invalid texture path')
        count=len(mesh['triangles'])*3
        if not count or count>500000: raise ValueError('Vertex count out of range')
        output+=struct.pack('<I',len(texture))+texture+struct.pack('<I',count)
        for tri in mesh['triangles']:
            for index in tri:
                if index<0 or index>=len(mesh['vertices']): raise ValueError('Triangle index out of range')
                values=mesh['vertices'][index]+mesh['normals'][index]+mesh['uvs'][index]+mesh['colors'][index]
                if len(values)!=12 or not all(math.isfinite(v) for v in values): raise ValueError('Invalid vertex')
                output+=struct.pack('<12f',*values)
    return output

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('scene',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    data=encode(json.loads(a.scene.read_text()));a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(data)
    print(a.output,len(data),'bytes')
