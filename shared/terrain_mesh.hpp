#pragma once
#include "protocol.hpp"
#include "transform.hpp"
#include <vector>
namespace bridge {
constexpr std::uint32_t kTerrainMagic=0x4d544c52; // RLTM
constexpr std::uint32_t kTerrainLimit=250000;
// Keep native RL camera calculations above the stock arena floor. Wire coordinates
// remain unchanged so Skyrim calibration, resets and recordings are continuous.
// A 10,000-unit lift still reaches RL's kill plane on long mountain descents.
// Leave 100,000 RL units (about 70,000 Skyrim units at default scale) of headroom.
// Share this offset with terrain and moving-body contacts; lifting only the car
// would detach it from both collision worlds.
constexpr Vec3 kNativeTerrainOffset{0,0,100000};
struct TerrainHeader {
  std::uint32_t magic=kTerrainMagic,version=1;
  std::uint64_t session{},generation{};
  std::uint32_t triangles{},reserved{};
};
struct TerrainTriangle { Vec3 a,b,c; };
static_assert(sizeof(TerrainHeader)==32 && sizeof(TerrainTriangle)==36);
inline bool valid(const TerrainHeader& h) {return h.magic==kTerrainMagic && h.version==1 && h.session && h.generation && h.triangles && h.triangles<=kTerrainLimit && !h.reserved;}
inline bool valid(const TerrainTriangle& t) {
  if(!valid(t.a)||!valid(t.b)||!valid(t.c))return false;
  for(auto p:{t.a,t.b,t.c})if(length(p)>1e7f)return false;
  const auto u=t.b-t.a,v=t.c-t.a;
  const Vec3 n{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
  return length(n)>0.0001f;
}
}
