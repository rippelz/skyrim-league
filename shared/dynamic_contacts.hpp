#pragma once
#include "transform.hpp"
#include <vector>
namespace bridge {
constexpr std::uint32_t kDynamicMagic=0x59444c52, kContactMagic=0x43444c52;
constexpr std::uint32_t kDynamicLimit=256, kDynamicVertexLimit=65536, kContactLimit=2048;
constexpr std::uint32_t kCharacterBody=1;
struct DynamicHeader {
  std::uint32_t magic=kDynamicMagic,version=1;
  std::uint64_t session{},generation{},ack{};
  std::uint32_t bodies{},vertices{};
};
// All geometric/velocity quantities are in wire RL units, masses in engine kg.
// IDs are opaque, session-scoped tokens, never addresses received from a file.
struct DynamicBody {
  std::uint64_t id{},shape{};
  Body state;
  float mass{},friction{.6f},restitution{.15f};
  std::uint32_t flags{},first{},count{},reserved{};
};
struct ContactHeader {
  std::uint32_t magic=kContactMagic,version=1;
  std::uint64_t session{},generation{},snapshot{};
  std::uint32_t count{},reserved{};
};
struct DynamicContact {
  std::uint64_t id{};
  Vec3 point{},impulse{}; // impulse ON the Skyrim body, includes tangential friction
  float closing{};
  std::uint32_t source{}; // 0 car, 1 ball
};
static_assert(sizeof(DynamicHeader)==40 && sizeof(DynamicBody)==96);
static_assert(sizeof(ContactHeader)==40 && sizeof(DynamicContact)==40);
inline bool valid(const DynamicHeader& h) {
  return h.magic==kDynamicMagic && h.version==1 && h.session && h.generation && h.bodies<=kDynamicLimit && h.vertices<=kDynamicVertexLimit;
}
inline bool valid(const DynamicBody& b,std::uint32_t vertices) {
  return b.id && b.shape && valid(b.state) && finite(b.mass) && b.mass>=0 && b.mass<=1e6f &&
    finite(b.friction) && b.friction>=0 && b.friction<=10 && finite(b.restitution) && b.restitution>=0 && b.restitution<=1 &&
    !(b.flags&~kCharacterBody) && !b.reserved && b.count>=4 && b.count<=4096 && b.first<=vertices && b.count<=vertices-b.first;
}
inline bool valid(const ContactHeader& h) {
  return h.magic==kContactMagic && h.version==1 && h.session && h.generation && h.snapshot && h.count<=kContactLimit && !h.reserved;
}
inline bool valid(const DynamicContact& c) {
  return c.id && valid(c.point) && valid(c.impulse) && length(c.impulse)<1e8f && finite(c.closing) && c.closing>=0 && c.closing<1e6f && c.source<=1;
}
}
