#!/usr/bin/env python3
"""Convert UModel PSK geometry to Skyrim model axes and a NIF-builder JSON scene."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import struct

def read_psk(path):
    raw=path.read_bytes();at=0;chunks={}
    while at<len(raw):
        if len(raw)-at<32:raise ValueError('Truncated PSK header')
        name,_,size,count=struct.unpack_from('<20s3i',raw,at);at+=32
        if size<0 or count<0 or size*count>len(raw)-at:raise ValueError('Invalid PSK chunk')
        chunks[name.rstrip(b'\0').decode()]=(size,count,raw[at:at+size*count]);at+=size*count
    points=list(struct.iter_unpack('<3f',chunks['PNTS0000'][2]))
    wedges=list(struct.iter_unpack('<I2fBBH',chunks['VTXW0000'][2]))
    materials=[x[:64].split(b'\0')[0].decode() for x in (chunks['MATT0000'][2][i:i+88] for i in range(0,len(chunks['MATT0000'][2]),88))]
    colors=chunks.get('VERTEXCOLOR',(0,0,b''))[2]
    faces=list(struct.iter_unpack('<3HBBI',chunks['FACE0000'][2])) if 'FACE0000' in chunks else list(struct.iter_unpack('<3IBBI',chunks['FACE3200'][2]))
    return points,wedges,faces,materials,colors

def convert(path, textures, scale=1/1.43):
    points,wedges,faces,materials,colors=read_psk(path)
    grouped=defaultdict(list)
    for f in faces:
        if any(i>=len(wedges) for i in f[:3]):raise ValueError('PSK triangle index out of range')
        grouped[f[3]].append(f[:3])
    meshes=[]
    for material,triangles in grouped.items():
        used=sorted({i for face in triangles for i in face});indices={w:i for i,w in enumerate(used)}
        vertices=[];uv=[];vertex_colors=[]
        for w in used:
            point,u,v,*_=wedges[w]
            if point>=len(points):raise ValueError('PSK point index out of range')
            x,y,z=points[point]
            # UModel already reflects UE3 Y into PSK. A +90deg Z rotation now
            # puts UE3 forward (+X) on Skyrim model forward (+Y). No new reflection.
            vertices.append([-y*scale,x*scale,z*scale]);uv.append([u,v])
            vertex_colors.append([c/255 for c in colors[w*4:w*4+4]] if colors else [1,1,1,1])
        # PSK faces preserve UE winding. Reverse it for the right-handed target;
        # otherwise convex surfaces receive inward normals and incorrect lighting.
        tris=[[indices[f[2]],indices[f[1]],indices[f[0]]] for f in triangles];normals=[[0.,0.,0.] for _ in vertices]
        for a,b,c in tris:
            ab=[vertices[b][i]-vertices[a][i] for i in range(3)];ac=[vertices[c][i]-vertices[a][i] for i in range(3)]
            n=[ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]]
            for v in (a,b,c):
                for i in range(3):normals[v][i]+=n[i]
        for n in normals:
            length=math.sqrt(sum(x*x for x in n))
            if length>1e-12:
                for i in range(3):n[i]/=length
        name=materials[material]
        texture,normal=textures.get(name,('textures/rocketbridge/white.dds','textures/rocketbridge/flat_n.dds'))
        meshes.append(dict(name=name,vertices=vertices,triangles=tris,normals=normals,uvs=uv,colors=vertex_colors,texture=texture.replace("/","\\"),normal=normal.replace("/","\\")))
    return dict(meshes=meshes,source=str(path),scale=scale)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('psk',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--material',action='append',default=[],help='Name=diffuse DDS,normal DDS (Skyrim paths)')
    a=p.parse_args();textures={}
    for value in a.material:
        name,paths=value.split('=',1);diffuse,normal=paths.split(',',1);textures[name]=(diffuse,normal)
    scene=convert(a.psk,textures);a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(scene,separators=(',',':'))+'\n')
    print(a.output,[(m['name'],len(m['vertices']),len(m['triangles'])) for m in scene['meshes']])

if __name__=='__main__':main()
