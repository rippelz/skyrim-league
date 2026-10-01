#!/usr/bin/env python3
"""Convert a world-space Skyrim OBJ into the bridge's Rocket League coordinates."""
import argparse
import math
from pathlib import Path

def triple(value):
    try:
        v = tuple(float(x) for x in value.split(','))
        if len(v) == 3 and all(math.isfinite(x) for x in v):
            return v
    except ValueError:
        pass
    raise argparse.ArgumentTypeError('use three finite numbers: x,y,z')

def inverse_direction(v, yaw):
    c, s = math.cos(yaw), math.sin(yaw)
    x, y, z = v
    return (-s*x+c*y, c*x+s*y, z)

def convert(source, output, sky_origin, rl_origin, scale, yaw):
    with Path(source).open() as src, Path(output).open('x') as dst:
        dst.write('# RocketSkyrim collision mesh: RL units, axis reflection, corrected winding\n')
        for line in src:
            values = line.split()
            if not values:
                dst.write(line);continue
            if values[0] in ('v', 'vn'):
                v = tuple(float(x) for x in values[1:4])
                if len(v) != 3 or not all(math.isfinite(x) for x in v):
                    raise ValueError('invalid OBJ vector')
                if values[0] == 'v':
                    v = inverse_direction(tuple(v[i]-sky_origin[i] for i in range(3)), yaw)
                    v = tuple(rl_origin[i]+v[i]/scale for i in range(3))
                else:
                    v = inverse_direction(v, yaw)
                dst.write(values[0]+' '+' '.join(f'{x:.9g}' for x in v)+(' '+' '.join(values[4:]) if len(values)>4 else '')+'\n')
            elif values[0] == 'f':
                # Reflection changes handedness. Reverse the polygon, retaining UV/normal indexes.
                dst.write('f '+' '.join(reversed(values[1:]))+'\n')
            else:
                dst.write(line)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path);p.add_argument('output', type=Path)
    p.add_argument('--sky-origin', type=triple, required=True);p.add_argument('--rl-origin', type=triple, default=(0,0,0))
    p.add_argument('--scale', type=float, default=1/1.43);p.add_argument('--yaw-degrees', type=float, default=0)
    a = p.parse_args()
    if not math.isfinite(a.scale) or a.scale <= 0 or not math.isfinite(a.yaw_degrees):
        p.error('scale must be positive and yaw finite')
    convert(a.source,a.output,a.sky_origin,a.rl_origin,a.scale,math.radians(a.yaw_degrees))
    print(a.output)

if __name__ == '__main__':
    main()
