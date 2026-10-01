#!/usr/bin/env python3
"""Capture, inspect, replay and synthesize RocketSkyrim UDP packets without games."""
import argparse
import json
import math
from pathlib import Path
import secrets
import socket
import sys
import time
from protocol import State, Body, Camera, CAR, BALL, CAMERA, GROUND, FRAME, RECORD_MAGIC, STATE_PORT, frames, unpack_state

def positive(value):
    n = float(value)
    if not math.isfinite(n) or n <= 0:
        raise argparse.ArgumentTypeError('must be finite and positive')
    return n

def port(value):
    n = int(value)
    if not 1024 <= n <= 65535:
        raise argparse.ArgumentTypeError('port must be 1024..65535')
    return n

def now_us():
    return time.monotonic_ns() // 1000

def capture(a):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', a.port)); s.settimeout(0.25)
    forward = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if a.forward_port else None
    if a.forward_port == a.port:
        raise ValueError('forward port must differ from listen port')
    output = a.output.open('xb') if a.output else None
    if output:
        output.write(RECORD_MAGIC)
    start = now_us(); last_print = 0; count = rejected = 0; first = None
    print(f'Listening 127.0.0.1:{a.port}; Ctrl+C stops', flush=True)
    try:
        while not a.seconds or now_us()-start < a.seconds*1e6:
            try:
                data, source = s.recvfrom(65535)
            except socket.timeout:
                continue
            if source[0] != '127.0.0.1':
                continue
            try:
                p = unpack_state(data)
            except ValueError:
                rejected += 1
                continue
            now = now_us(); count += 1
            if first is None:
                first = now
            if output:
                output.write(FRAME.pack(now-first, len(data))); output.write(data)
            if forward:
                forward.sendto(data, ('127.0.0.1', a.forward_port))
            if a.json or now-last_print > 1e6:
                print(json.dumps(p.json()) if a.json else f'#{p.sequence} car={tuple(round(x, 1) for x in p.car.position)} boost={p.boost:.1f} flags={p.flags}', flush=True)
                last_print = now
    finally:
        if output:
            output.close()
        s.close()
        if forward:
            forward.close()
        print(f'{count} packets; {rejected} invalid', flush=True)

def inspect(a):
    count = 0; start = end = None; sessions = set()
    with a.file.open('rb') as file:
        for timestamp, data in frames(file):
            p = unpack_state(data); count += 1; sessions.add(p.session)
            if start is None:
                start = timestamp
            end = timestamp
    duration = (end-start)/1e6 if count else 0
    print(json.dumps({'packets': count, 'duration_seconds': duration, 'sessions': len(sessions),
                      'average_hz': (count-1)/duration if duration else 0}, indent=2))

def replay(a):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        while True:
            with a.file.open('rb') as file:
                epoch = time.monotonic(); first = None; sequence = 0; session = secrets.randbits(64) or 1
                for timestamp, data in frames(file):
                    if first is None:
                        first = timestamp
                    wait = epoch+(timestamp-first)/1e6/a.speed-time.monotonic()
                    if wait > 600:
                        raise ValueError('recording contains a gap longer than ten minutes')
                    if wait > 0:
                        time.sleep(wait)
                    # Every replay loop is a new sender; remap sequence and timestamp.
                    p = unpack_state(data); p.session = session; sequence += 1; p.sequence = sequence & 0xffffffff
                    p.timestamp_us = now_us()
                    s.sendto(p.pack(), ('127.0.0.1', a.port))
            if first is None:
                raise ValueError('empty recording')
            print(f'Replayed {sequence} packets', flush=True)
            if not a.loop:
                return

def demo_state(seconds, sequence, session, timestamp):
    speed = 2*math.pi/15; phase = seconds*speed; radius = 450
    yaw = phase+math.pi/2
    position = (radius*(math.cos(phase)-1), radius*math.sin(phase), 17+120*max(0, math.sin(phase*2))**8)
    rotation = (0, 0, math.sin(yaw/2), math.cos(yaw/2))
    car = Body(position, rotation, (-radius*speed*math.sin(phase), radius*speed*math.cos(phase), 0), (0,0,speed))
    camera_pos = (position[0]-300*math.cos(yaw), position[1]-300*math.sin(yaw), position[2]+150)
    pitch = math.atan2(150, 300)
    camera_q = (-math.sin(yaw/2)*math.sin(pitch/2), math.cos(yaw/2)*math.sin(pitch/2), math.sin(yaw/2)*math.cos(pitch/2), math.cos(yaw/2)*math.cos(pitch/2))
    return State(sequence & 0xffffffff, session, timestamp, CAR|BALL|CAMERA|GROUND,
                 car, Body((0, 700, 93)), Camera(camera_pos, camera_q, 100), 80)

def demo(a):
    session = secrets.randbits(64) or 1; count = 0
    output = a.output.open('xb') if a.output else None
    if output:
        output.write(RECORD_MAGIC)
    print(f'Demo -> 127.0.0.1:{a.port} at {a.hz:g} Hz', flush=True)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            epoch = time.monotonic()
            while not a.seconds or time.monotonic()-epoch < a.seconds:
                elapsed = time.monotonic()-epoch; count += 1
                data = demo_state(elapsed, count, session, now_us()).pack()
                s.sendto(data, ('127.0.0.1', a.port))
                if output:
                    output.write(FRAME.pack(int(elapsed*1e6), len(data)));output.write(data)
                wait = epoch+count/a.hz-time.monotonic()
                if wait > 0:
                    time.sleep(wait)
    finally:
        if output:
            output.close()
        print(f'{count} demo packets', flush=True)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    s = sub.add_parser('capture', help='sniff/record or forward to a second SKSE port')
    s.add_argument('--port', type=port, default=STATE_PORT);s.add_argument('--forward-port', type=port)
    s.add_argument('--output', type=Path);s.add_argument('--seconds', type=positive);s.add_argument('--json', action='store_true');s.set_defaults(func=capture)
    s = sub.add_parser('inspect', help='validate a recording and show its stats')
    s.add_argument('file', type=Path);s.set_defaults(func=inspect)
    s = sub.add_parser('replay', help='feed a recording into the SKSE listener')
    s.add_argument('file', type=Path);s.add_argument('--port', type=port, default=STATE_PORT)
    s.add_argument('--speed', type=positive, default=1);s.add_argument('--loop', action='store_true');s.set_defaults(func=replay)
    s = sub.add_parser('demo', help='feed a circular car/ball scene into Skyrim')
    s.add_argument('--port', type=port, default=STATE_PORT);s.add_argument('--hz', type=positive, default=120)
    s.add_argument('--seconds', type=positive);s.add_argument('--output', type=Path);s.set_defaults(func=demo)
    a = p.parse_args()
    try:
        a.func(a)
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError) as error:
        p.exit(1, f'bridge: {error}\n')

if __name__ == '__main__':
    main()
