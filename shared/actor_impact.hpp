#pragma once
#include "transform.hpp"
namespace bridge {
inline Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline float dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
// Sweep an actor capsule's conservative bounds against the car's oriented box.
// Ball callers supply equal half extents. Sweeping avoids skipping a pedestrian
// between render updates; callers reject teleport/re-anchor segments.
inline bool sweep_actor(Vec3 from,Vec3 to,Quat rotation,Vec3 extents,Vec3 actor,float radius=22,float half_height=40,float* fraction=nullptr,Vec3* normal=nullptr) {
  const Quat inverse{-rotation.x,-rotation.y,-rotation.z,rotation.w};
  const auto a=rotate(inverse,actor-from),b=rotate(inverse,actor-to),d=b-a;
  const auto up=rotate(inverse,{0,0,half_height});
  extents=extents+Vec3{radius+std::abs(up.x),radius+std::abs(up.y),radius+std::abs(up.z)};
  float lo=0,hi=1;Vec3 face{};
  const float starts[]={a.x,a.y,a.z},deltas[]={d.x,d.y,d.z},bounds[]={extents.x,extents.y,extents.z};
  for(int i=0;i<3;++i){
    if(std::abs(deltas[i])<1e-6f){if(std::abs(starts[i])>bounds[i])return false;continue;}
    auto entry=(-bounds[i]-starts[i])/deltas[i],exit=(bounds[i]-starts[i])/deltas[i];
    if(entry>exit)std::swap(entry,exit);if(entry>lo){face={};if(i==0)face.x=deltas[i]<0?1.0f:-1.0f;if(i==1)face.y=deltas[i]<0?1.0f:-1.0f;if(i==2)face.z=deltas[i]<0?1.0f:-1.0f;}lo=std::max(lo,entry);hi=std::min(hi,exit);if(lo>hi)return false;
  }
  if(fraction)*fraction=lo;
  if(normal)*normal=rotate(rotation,face);
  return true;
}
struct ActorImpact {float damage{},launch_speed{},body_recoil{};};
inline ActorImpact actor_impact(float closing_rl,float mass,float damage_scale) {
  if(!finite(closing_rl) || !finite(mass) || !finite(damage_scale) || closing_rl<250 || mass<=0 || damage_scale<0)return {};
  const auto speed=std::min(closing_rl,6000.0f);
  constexpr float actor_mass=80,restitution=.25f;
  const float impulse=(1+restitution)*speed/(1/mass+1/actor_mass);
  return {std::min(500.0f,.5f*mass*(speed/100)*(speed/100)*damage_scale),impulse/actor_mass,impulse/mass};
}
}
