#pragma once
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bridge {
static_assert(std::endian::native == std::endian::little, "Wire protocol requires little endian");
constexpr std::uint32_t kMagic = 0x42534c52; // bytes: R L S B
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kStatePort = 29741, kEventPort = 29742;
enum class Kind : std::uint16_t { State = 1, Event = 2 };
enum Flag : std::uint32_t {
  CarValid = 1, BallValid = 2, CameraValid = 4, OnGround = 8,
  Supersonic = 16, Demolished = 32, Boosting = 64
};
enum class EventType : std::uint32_t { SetBall = 1, TeleportCar = 2, BumpCar = 3, CarSurface = 4, BallSurface = 5, BridgeActive = 6, Wheel0 = 7, Wheel1 = 8, Wheel2 = 9, Wheel3 = 10, WheelMiss0 = 11, WheelMiss1 = 12, WheelMiss2 = 13, WheelMiss3 = 14, BumpBall = 15, Interact = 16, PauseDriving = 17, ActorContactCar = 18, ActorContactBall = 19 };
#pragma pack(push, 1)
struct Vec3 { float x{}, y{}, z{}; };
struct Quat { float x{}, y{}, z{}, w{1}; };
struct Header {
  std::uint32_t magic{kMagic};
  std::uint16_t version{kVersion};
  Kind kind{Kind::State};
  std::uint32_t bytes{};
  std::uint32_t sequence{};
  std::uint64_t session{};
  std::uint64_t timestamp_us{}; // sender's monotonic time, not wall clock
};
struct Body { Vec3 position; Quat rotation; Vec3 velocity; Vec3 angular_velocity; };
struct Camera { Vec3 position; Quat rotation; float fov{90}; };
struct StatePacket { Header header; std::uint32_t flags{}; Body car; Body ball; Camera camera; float boost{}; };
struct EventPacket {
  Header header;
  EventType type{EventType::BumpCar};
  std::uint32_t target_sequence{};
  Vec3 position;
  Vec3 velocity; // SetBall/TeleportCar: absolute velocity; BumpCar: delta velocity
  Quat rotation;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 32 && sizeof(Body) == 52);
static_assert(sizeof(StatePacket) == 176 && sizeof(EventPacket) == 80);
static_assert(offsetof(StatePacket, car) == 36 && offsetof(StatePacket, boost) == 172);
static_assert(std::is_trivially_copyable_v<StatePacket>);

inline bool finite(float v, float bound = 1.0e7f) { return std::isfinite(v) && std::abs(v) <= bound; }
inline bool valid(Vec3 v, float bound = 1.0e7f) { return finite(v.x, bound) && finite(v.y, bound) && finite(v.z, bound); }
inline bool valid(Quat q) {
  if (!finite(q.x, 2) || !finite(q.y, 2) || !finite(q.z, 2) || !finite(q.w, 2)) return false;
  const auto n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
  return n >= 0.25f && n <= 4.0f;
}
inline bool valid(const Body& b) { return valid(b.position) && valid(b.rotation) && valid(b.velocity, 1e6f) && valid(b.angular_velocity, 1e4f); }
inline bool valid(const Header& h, Kind kind, std::size_t size) {
  return h.magic == kMagic && h.version == kVersion && h.kind == kind && h.bytes == size && h.session != 0;
}
inline bool valid(const StatePacket& p) {
  return valid(p.header, Kind::State, sizeof p) && !(p.flags & ~127u) &&
    (!(p.flags & CarValid) || valid(p.car)) && (!(p.flags & BallValid) || valid(p.ball)) &&
    (!(p.flags & CameraValid) || (valid(p.camera.position) && valid(p.camera.rotation) &&
      finite(p.camera.fov, 179) && p.camera.fov >= 30)) && finite(p.boost, 100) && p.boost >= 0;
}
inline bool valid(const EventPacket& p) {
  const auto type = static_cast<std::uint32_t>(p.type);
  return valid(p.header, Kind::Event, sizeof p) && type >= 1 && type <= 19 &&
    valid(p.position) && valid(p.velocity, 1e5f) && valid(p.rotation);
}
template<class Packet> bool decode(const void* data, std::size_t size, Packet& out) {
  if (size != sizeof(Packet)) return false;
  Packet p;
  std::memcpy(&p, data, sizeof p);
  if (!valid(p)) return false;
  out = p;
  return true;
}
// Feedback timestamps survive a Skyrim process restart, unlike its counter.
inline bool newer_event(const Header& incoming, std::uint64_t last_time) { return incoming.timestamp_us > last_time; }
inline bool newer(std::uint32_t a, std::uint32_t b) { const auto d = a - b; return d != 0 && d < 0x80000000u; }
}
