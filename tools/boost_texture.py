"""Generate a small original RGBA boost flame texture; no game assets required."""
import math
import struct

def flame_dds(width=64,height=128):
    pixels=bytearray()
    for y in range(height):
        t=y/(height-1)
        for x in range(width):
            across=abs(2*x/(width-1)-1)
            width_at_t=.92*(1-t)**.65+.015
            density=max(0,1-across/width_at_t)
            streak=.82+.18*math.sin(t*65+x*.42)
            alpha=int(255*density**1.5*(1-t)**.45*streak)
            core=density*(1-t)
            pixels.extend((255,int(65+185*core),int(8+230*core**3),alpha))
    header=[124,0x100F,height,width,width*4,0,0]+[0]*11+[32,0x41,0,32,0xFF,0xFF00,0xFF0000,0xFF000000]+[0x1000,0,0,0,0]
    return b'DDS '+struct.pack('<31I',*header)+pixels
