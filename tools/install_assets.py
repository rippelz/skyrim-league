#!/usr/bin/env python3
"""Build and install private Fennec/ball NIFs exported from the local RL installation."""
import configparser
import copy
import json
from pathlib import Path
import struct
import subprocess
from install import Installer
from psk_to_scene import convert,read_psk
from build_render_mesh import encode
from boost_texture import flame_dds

ROOT=Path(__file__).resolve().parents[1]

def flat_dds(rgba):
    # Uncompressed RGBA DDS, used only for neutral fallback materials.
    header=[124,0x100F,4,4,16,0,0]+[0]*11+[32,0x41,0,32,0xFF,0xFF00,0xFF0000,0xFF000000]+[0x1000,0,0,0,0]
    return b'DDS '+struct.pack('<31I',*header)+bytes(rgba)*16

def wheel_positions(psk):
    raw=psk.read_bytes();at=0
    while at<len(raw):
        name,_,size,count=struct.unpack_from('<20s3i',raw,at);at+=32
        if name.rstrip(b'\0')==b'REFSKELT':
            positions=[];rotations=[];result={}
            def rotate(q,v):
                x,y,z,w=q;tx=2*(y*v[2]-z*v[1]);ty=2*(z*v[0]-x*v[2]);tz=2*(x*v[1]-y*v[0])
                return [v[0]+w*tx+y*tz-z*ty,v[1]+w*ty+z*tx-x*tz,v[2]+w*tz+x*ty-y*tx]
            def multiply(a,b):
                x,y,z,w=a;X,Y,Z,W=b
                return [w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z]
            for i in range(count):
                data=raw[at+i*size:at+(i+1)*size];bone=data[:64].split(b'\0')[0].decode()
                parent=struct.unpack_from('<i',data,72)[0];q=struct.unpack_from('<4f',data,76)
                pos=struct.unpack_from('<3f',data,92)
                if i:
                    pos=rotate(rotations[parent],pos)
                    pos=[pos[j]+positions[parent][j] for j in range(3)]
                    q=multiply(rotations[parent],q)
                positions.append(pos)
                rotations.append(q)
                if bone.endswith('_Disc_jnt'):result[bone[:2]]=[-pos[1]/1.43,pos[0]/1.43,pos[2]/1.43]
            return result
        at+=size*count
    raise ValueError('Missing Fennec skeleton')

def main():
    state=json.loads((ROOT/'build/install-state.json').read_text());game=Path(state['skyrim'])
    body=ROOT/'build/assets/rl/body_grain_SF/SkeletalMesh3/Body_Grain_SK.psk'
    wheel=ROOT/'build/assets/rl/wheel_oemplus_SF/StaticMesh3/SM_wheel_rl204.pskx'
    ball=ROOT/'build/assets/rl/GameInfo_Soccar_SF/StaticMesh3/Ball_DefaultBall00.pskx'
    tex=lambda d,n:('textures/rocketbridge/'+d.lower()+'.dds','textures/rocketbridge/'+n.lower()+'.dds')
    car=convert(body,{'MIC_Body_Grain':tex('Body_Grain_D','Body_Grain_BodyNormalMap'),
                      'MIC_Chassis_Grain':tex('Chassis_Grain_D','Chassis_Grain_N')})
    lenses=convert(ROOT/'build/assets/rl/body_grain_SF/StaticMesh3/Body_Grain_Lenses_SM.pskx',{})
    # UModel's glTF material export gives a neutral 0.3 lens factor. The
    # geometry is exact; UE3's reflective lens shader is not recreated here.
    for part in lenses['meshes']:
        part['colors']=[[0.3,0.3,0.3,1] for _ in part['vertices']]
        car['meshes'].append(part)
    wheels=convert(wheel,{'MIC_Wheel_OEMplus':tex('wheel_rl204_D','wheel_rl204_N')})
    for name,pos in wheel_positions(body).items():
        for original in wheels['meshes']:
            part=copy.deepcopy(original);part['name']+='_'+name
            # Use rigid OEM+ visuals at the Fennec's actual hub positions. Exact
            # wheel loadout, steering and suspension animation are a later stream.
            side=-1 if name.endswith('R') else 1
            part['vertices']=[[v[0]*side+pos[0],v[1]+pos[1],v[2]+pos[2]] for v in part['vertices']]
            if side<0:
                part['normals']=[[-v[0],v[1],v[2]] for v in part['normals']]
                part['triangles']=[[t[1],t[0],t[2]] for t in part['triangles']]
            car['meshes'].append(part)
    ball_scene=convert(ball,{'MAT_Ball_V3':tex('Ball_Default00_D','Ball_Default00_N')})
    ao_tool=ROOT/'build/assets/bake_mesh_ao'
    subprocess.run(['c++','-O3','-std=c++20',str(ROOT/'tools/bake_mesh_ao.cpp'),'-o',str(ao_tool)],check=True)
    install=Installer()
    install.put(game/"Data/textures/rocketbridge/boost_flame.dds",flame_dds())
    for name,scene in [('Fennec',car),('Ball',ball_scene)]:
        source=ROOT/f'build/assets/{name}.json';source.write_text(json.dumps(scene,separators=(',',':')))
        raw=ROOT/f'build/assets/{name}-raw.rmesh';raw.write_bytes(encode(scene))
        shaded=ROOT/f'build/assets/{name}-ao.rmesh'
        subprocess.run([str(ao_tool),str(raw),str(shaded)],check=True)
        install.put(game/f'Data/SKSE/Plugins/RocketBridge-{("Car" if name=="Fennec" else "Ball")}.rmesh',shaded.read_bytes())
        nif=ROOT/f'build/assets/nifs/{name}.nif';nif.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run([str(ROOT/'build/assets/build_nif'),str(source),str(nif)],check=True)
        # Skyrim's resource lookup canonicalizes names to lowercase. Keep
        # loose files canonical on Proton's case-sensitive host filesystem.
        install.copy(nif,game/f'Data/meshes/rocketbridge/{name.lower()}.nif')
    install.copy(ROOT/"build/assets/rl/body_grain_SF/Texture2D/Body_Grain_BlankSkin.dds",game/"Data/textures/rocketbridge/body_grain_blankskin.dds")
    required={Path(path.replace("\\","/")).name for scene in (car,ball_scene) for mesh in scene['meshes'] for path in (mesh['texture'],mesh['normal'])}
    for name in required:
        target=game/'Data/textures/rocketbridge'/name
        if name in ('white.dds','flat_n.dds'):
            install.put(target,flat_dds((255,255,255,255) if name=='white.dds' else (128,128,255,255)))
        else:
            candidates=[p for p in (ROOT/'build/assets/rl').rglob('*.dds') if p.name.lower()==name]
            if len(candidates)!=1:raise ValueError(f'Missing or ambiguous texture: {name}')
            install.copy(candidates[0],target)
    ini=game/'Data/SKSE/Plugins/SkyrimRocketBridge.ini'
    text=ini.read_text()
    import re
    for key,value in [('CarModel',r'rocketbridge\fennec.nif'),('BallModel',r'rocketbridge\ball.nif'),('CarScale','1'),('BallScale','1')]:
        text=re.sub(r'^'+key+r'=.*$',lambda _:key+'='+value,text,flags=re.M)
    if 'AnchorCarClearance=' not in text:text+='\nAnchorCarClearance=17\n'
    install.put(ini,text.encode());install.save()
    print('Installed Fennec, rigid OEM+ wheels and real standard ball. Private local assets only.')

if __name__=='__main__':main()
