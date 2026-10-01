"""Wire-compatible with shared/protocol.hpp. Python standard library only."""
from dataclasses import dataclass, field, asdict
import math
import struct

MAGIC, VERSION, STATE_PORT, EVENT_PORT = 0x42534C52, 1, 29741, 29742
CAR, BALL, CAMERA, GROUND, SUPERSONIC, DEMOLISHED = 1, 2, 4, 8, 16, 32
BOOSTING = 64
STATE = struct.Struct('<IHHIIQQI' + '13f'*2 + '8f' + 'f')
EVENT = struct.Struct('<IHHIIQQII3f3f4f')
FRAME = struct.Struct('<QI')
RECORD_MAGIC = b'RLSBREC1'

@dataclass
class Body:
    position: tuple = (0.0, 0.0, 0.0)
    rotation: tuple = (0.0, 0.0, 0.0, 1.0)
    velocity: tuple = (0.0, 0.0, 0.0)
    angular_velocity: tuple = (0.0, 0.0, 0.0)
    def floats(self):
        return (*self.position, *self.rotation, *self.velocity, *self.angular_velocity)

@dataclass
class Camera:
    position: tuple = (0.0, 0.0, 0.0)
    rotation: tuple = (0.0, 0.0, 0.0, 1.0)
    fov: float = 90.0

@dataclass
class State:
    sequence: int = 0
    session: int = 1
    timestamp_us: int = 0
    flags: int = CAR | BALL | CAMERA
    car: Body = field(default_factory=Body)
    ball: Body = field(default_factory=Body)
    camera: Camera = field(default_factory=Camera)
    boost: float = 100.0
    def pack(self):
        return STATE.pack(MAGIC, VERSION, 1, STATE.size, self.sequence, self.session,
                          self.timestamp_us, self.flags, *self.car.floats(), *self.ball.floats(),
                          *self.camera.position, *self.camera.rotation, self.camera.fov, self.boost)
    def json(self):
        return asdict(self)

def finite(values, bound):
    return all(math.isfinite(v) and abs(v) <= bound for v in values)

def valid_quat(q):
    return finite(q, 2) and 0.25 <= sum(v*v for v in q) <= 4.0

def valid_body(b):
    return finite(b.position, 1e7) and valid_quat(b.rotation) and finite(b.velocity, 1e6) and finite(b.angular_velocity, 1e4)

def unpack_state(data):
    if len(data) != STATE.size:
        raise ValueError(f'state length {len(data)} != {STATE.size}')
    v = STATE.unpack(data)
    if v[:4] != (MAGIC, VERSION, 1, STATE.size) or not v[5] or v[7] & ~127:
        raise ValueError('invalid state header')
    def body(i):
        return Body(v[i:i+3], v[i+3:i+7], v[i+7:i+10], v[i+10:i+13])
    p = State(v[4], v[5], v[6], v[7], body(8), body(21), Camera(v[34:37], v[37:41], v[41]), v[42])
    if ((p.flags & CAR and not valid_body(p.car)) or (p.flags & BALL and not valid_body(p.ball))
        or (p.flags & CAMERA and not (finite(p.camera.position, 1e7) and valid_quat(p.camera.rotation)
                                     and math.isfinite(p.camera.fov) and 30 <= p.camera.fov <= 179))
        or not math.isfinite(p.boost) or not 0 <= p.boost <= 100):
        raise ValueError('invalid state pose')
    return p

def event_packet(state, kind, position=(0, 0, 0), velocity=(0, 0, 0), rotation=(0, 0, 0, 1), sequence=1, timestamp=0):
    if kind not in range(1,15) or not finite(position, 1e7) or not finite(velocity, 1e5) or not valid_quat(rotation):
        raise ValueError('invalid event')
    return EVENT.pack(MAGIC, VERSION, 2, EVENT.size, sequence, state.session, timestamp,
                      kind, state.sequence, *position, *velocity, *rotation)

def frames(file):
    if file.read(8) != RECORD_MAGIC:
        raise ValueError('not an RLSBREC1 recording')
    previous = -1
    while True:
        header = file.read(FRAME.size)
        if not header:
            return
        if len(header) != FRAME.size:
            raise ValueError('truncated recording frame')
        timestamp, size = FRAME.unpack(header)
        if size != STATE.size or timestamp < previous:
            raise ValueError('invalid recording length/timing')
        packet = file.read(size)
        unpack_state(packet)
        previous = timestamp
        yield timestamp, packet
