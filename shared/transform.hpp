#pragma once
#include "protocol.hpp"
#include <algorithm>
#include <array>

namespace bridge {
constexpr float kPi = 3.14159265358979323846f;
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline float length(Vec3 v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
inline Quat normalized(Quat q) {
  auto n = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
  return n < 1e-8f ? Quat{} : Quat{q.x/n, q.y/n, q.z/n, q.w/n};
}
inline Quat multiply(Quat a, Quat b) {
  return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
          a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
          a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
          a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
inline Quat nlerp(Quat a, Quat b, float t) {
  if (a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w < 0) b = {-b.x,-b.y,-b.z,-b.w};
  return normalized({a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t});
}
using Matrix = std::array<std::array<float,3>,3>;
inline Matrix matrix(Quat raw) {
  const auto q = normalized(raw); auto x=q.x, y=q.y, z=q.z, w=q.w;
  return {{{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)},
           {2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)},
           {2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)}}};
}
inline Vec3 rotate(Quat q, Vec3 v) {
  const auto m=matrix(q);
  return {m[0][0]*v.x+m[0][1]*v.y+m[0][2]*v.z,
          m[1][0]*v.x+m[1][1]*v.y+m[1][2]*v.z,
          m[2][0]*v.x+m[2][1]*v.y+m[2][2]*v.z};
}
// Unreal: X forward, Y right, Z up (left handed). Skyrim: X east, Y north,
// Z up. Swapping X/Y reflects the basis; quaternion vector parts are axial.
// A car mesh should have its front along local +Y. Units are configurable.
struct Transform {
  float scale{1.0f/1.43f};
  float yaw{}; // radians, rotation of the mapped world around +Z
  Vec3 rl_origin{}, sky_origin{};
  Quat yaw_quat() const { return {0,0,std::sin(yaw/2),std::cos(yaw/2)}; }
  Vec3 direction(Vec3 rl) const { return rotate(yaw_quat(), {rl.y,rl.x,rl.z}); }
  Vec3 position(Vec3 rl) const { return sky_origin + direction(rl-rl_origin)*scale; }
  Vec3 velocity(Vec3 rl) const { return direction(rl)*scale; }
  Vec3 inverse_direction(Vec3 sky) const {
    const auto v = rotate({0,0,-std::sin(yaw/2),std::cos(yaw/2)},sky);
    return {v.y,v.x,v.z};
  }
  Vec3 inverse_position(Vec3 sky) const { return rl_origin + inverse_direction(sky-sky_origin)*(1.0f/scale); }
  Quat rotation(Quat q) const { return normalized(multiply(yaw_quat(), {-q.y,-q.x,-q.z,q.w})); }
};
// NiCamera columns are view direction, up, right. They are a different local
// basis from the car mesh and TES camera-state quaternion.
inline Matrix camera_matrix(const Transform& transform,Quat rl_rotation) {
  const auto forward=transform.direction(rotate(rl_rotation,{1,0,0}));
  const auto up=transform.direction(rotate(rl_rotation,{0,0,1}));
  const auto right=transform.direction(rotate(rl_rotation,{0,1,0}));
  return {{{forward.x,up.x,right.x},{forward.y,up.y,right.y},{forward.z,up.z,right.z}}};
}
inline Body interpolate(const Body& a, const Body& b, float t) {
  return {a.position+(b.position-a.position)*t, nlerp(a.rotation,b.rotation,t),
    a.velocity+(b.velocity-a.velocity)*t, a.angular_velocity+(b.angular_velocity-a.angular_velocity)*t};
}
inline StatePacket interpolate(const StatePacket& a, const StatePacket& b, float t) {
  auto p=b;
  // Never blend across respawns, teleports or disappearing objects.
  if ((a.flags&7u) != (b.flags&7u) || length(a.car.position-b.car.position)>1000) return b;
  p.car=interpolate(a.car,b.car,t);
  p.ball=length(a.ball.position-b.ball.position)>1000?b.ball:interpolate(a.ball,b.ball,t);
  p.camera.position=a.camera.position+(b.camera.position-a.camera.position)*t;
  p.camera.rotation=nlerp(a.camera.rotation,b.camera.rotation,t);
  p.camera.fov=a.camera.fov+(b.camera.fov-a.camera.fov)*t;
  p.boost=a.boost+(b.boost-a.boost)*t;
  return p;
}
}
