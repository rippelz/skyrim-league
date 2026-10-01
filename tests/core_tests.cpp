#include "shared/timeline.hpp"
#include "shared/terrain_mesh.hpp"
#include "shared/actor_impact.hpp"
#include "shared/shadow_projection.hpp"
#include "shared/udp.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>

using namespace bridge;
void require(bool b,const char* why) { if (!b) { std::cerr<<why<<'\n';std::exit(1); } }
bool near(Vec3 a,Vec3 b,float tolerance=0.01f) { return length(a-b)<tolerance; }
StatePacket state(std::uint32_t seq,std::uint64_t time,std::uint64_t session=7) {
  StatePacket p{};p.header={kMagic,kVersion,Kind::State,sizeof p,seq,session,time};
  p.flags=CarValid|BallValid|CameraValid;p.car.position={float(seq)*100,200,17};p.boost=80;
  return p;
}
int main(int argc,char** argv) {
  const float sun_uv[4][4]={{.01f,0,0,0},{0,.02f,0,0},{0,0,.001f,0},{.5f,.5f,.25f,1}};
  float sun_clip[4][4]{};
  require(shadow_clip_matrix(sun_uv,{10,20,30},sun_clip),"sun projection rejected");
  require(std::abs(sun_clip[0][3]-.2f)<.001f && std::abs(sun_clip[1][3]+.8f)<.001f && std::abs(sun_clip[2][3]-.28f)<.001f,"shadow camera rebase or UV clip conversion wrong");
  float transposed[4][4]{};for(int r=0;r<4;++r)for(int c=0;c<4;++c)transposed[r][c]=sun_uv[c][r];
  float column_clip[4][4]{};require(shadow_clip_matrix(transposed,{10,20,30},column_clip),"column projection rejected");
  for(int r=0;r<4;++r)for(int c=0;c<4;++c)require(std::abs(column_clip[r][c]-sun_clip[r][c])<.001f,"shadow matrix storage convention mismatch");

  require(sweep_actor({-200,0,45},{200,0,45},{},{60,40,20},{0,0,45}),"fast NPC sweep skipped actor");
  require(!sweep_actor({-200,200,45},{200,200,45},{},{60,40,20},{0,0,45}),"distant actor falsely hit");
  require(!sweep_actor({-200,0,250},{200,0,250},{},{60,40,20},{0,0,45}),"car above actor falsely hit");
  float contact_fraction{};
  require(sweep_actor({-200,0,45},{200,0,45},{},{60,40,20},{0,0,45},22,40,&contact_fraction) && std::abs(contact_fraction-.295f)<.001f,"NPC contact time incorrect");
  require(actor_impact(249,180,.004f).damage==0,"walking-speed contact damages actor");
  const auto slow=actor_impact(500,180,.004f),fast=actor_impact(1000,180,.004f);
  require(std::abs(fast.damage-4*slow.damage)<.01f,"impact damage does not follow energy");
  require(std::abs(fast.launch_speed*80-fast.body_recoil*180)<.01f,"impact momentum is not balanced");
  require(actor_impact(2300,180,.03f).damage>200,"high-speed car hit should be lethal for ordinary NPCs");
  require(actor_impact(1000,30,.03f).damage<actor_impact(1000,180,.03f).damage,"lighter ball damage exceeds car damage");
  TerrainHeader terrain{};terrain.session=7;terrain.generation=1;terrain.triangles=1;
  require(valid(terrain),"valid terrain header rejected");
  auto invalid_terrain=terrain;invalid_terrain.triangles=kTerrainLimit+1;require(!valid(invalid_terrain),"oversized terrain accepted");
  invalid_terrain=terrain;invalid_terrain.session=0;require(!valid(invalid_terrain),"sessionless terrain accepted");
  invalid_terrain=terrain;invalid_terrain.version=2;require(!valid(invalid_terrain),"unknown terrain version accepted");
  TerrainTriangle triangle{{0,0,0},{10,0,0},{0,10,0}};require(valid(triangle),"valid terrain triangle rejected");
  triangle.c=triangle.b;require(!valid(triangle),"degenerate terrain triangle accepted");
  triangle.c={0,10,std::numeric_limits<float>::infinity()};require(!valid(triangle),"nonfinite terrain accepted");
  auto p=state(1,100000);StatePacket decoded;
  require(decode(&p,sizeof p,decoded),"valid state rejected");
  require(!decode(&p,sizeof p-1,decoded),"truncated state accepted");
  auto bad=p;bad.header.version=99;require(!valid(bad),"unknown version accepted");
  bad=p;bad.car.position.x=std::numeric_limits<float>::quiet_NaN();require(!valid(bad),"NaN accepted");
  bad=p;bad.car.rotation={0,0,0,0};require(!valid(bad),"zero quaternion accepted");
  bad=p;bad.flags=0x800;require(!valid(bad),"unknown flags accepted");
  require(newer(0,0xffffffffu) && !newer(2,3) && !newer(2,2),"sequence wrap/order broken");
  EventPacket surface{};
  surface.header={kMagic,kVersion,Kind::Event,sizeof surface,1,7,1000};
  surface.type=EventType::CarSurface;surface.position={100,200,0};surface.velocity={0,0,1};
  EventPacket surface_decoded;
  require(decode(&surface,sizeof surface,surface_decoded),"car surface feedback rejected");
  require(newer_event(surface.header,999),"fresh feedback rejected");
  require(!newer_event(surface.header,1000),"duplicate feedback accepted");
  require(!newer_event(surface.header,1001),"delayed feedback accepted");
  surface.header.sequence=0;surface.header.timestamp_us=1002;
  require(newer_event(surface.header,1000),"Skyrim restart counter blocked feedback");
  surface.type=EventType::BallSurface;
  require(decode(&surface,sizeof surface,surface_decoded),"ball surface feedback rejected");
  surface.type=EventType::BridgeActive;require(valid(surface),"bridge heartbeat rejected");
  surface.type=EventType::Wheel3;require(valid(surface),"wheel contact rejected");
  surface.type=EventType::WheelMiss3;require(valid(surface),"wheel miss rejected");
  surface.type=EventType::ActorContactCar;require(valid(surface),"car actor contact rejected");
  surface.type=EventType::ActorContactBall;require(valid(surface),"ball actor contact rejected");
  surface.type=static_cast<EventType>(20);require(!valid(surface),"unknown contact type accepted");
  auto boosted=p;boosted.flags|=Boosting;require(valid(boosted),"boost flag rejected");
  Transform t;t.rl_origin={100,-50,17};t.sky_origin={-9000,3000,70};t.yaw=0.71f;
  require(near(t.inverse_position(t.position({1000,20,350})),{1000,20,350}),"position roundtrip broken");
  // Reflection must satisfy R_sky * C(v) == C * R_rl(v), not just swap Euler angles.
  for (const Quat q:{Quat{},normalized({0.2f,-0.3f,0.4f,0.8f})}) {
    for (const Vec3 v:{Vec3{1,0,0},Vec3{0,1,0},Vec3{0,0,1}}) {
      const auto converted=rotate(t.rotation(q),{v.y,v.x,v.z});
      require(near(converted,t.direction(rotate(q,v))),"handedness conversion broken");
    }
  }
  require(near(rotate(nlerp({0,0,0,1},{0,0,0,-1},0.5f),{1,0,0}),{1,0,0}),"shortest quaternion interpolation broken");
  const auto camera=camera_matrix(Transform{},Quat{});
  require(near({camera[0][0],camera[1][0],camera[2][0]},{0,1,0}),"camera view axis broken");
  require(near({camera[0][1],camera[1][1],camera[2][1]},{0,0,1}),"camera up axis broken");
  require(near({camera[0][2],camera[1][2],camera[2][2]},{1,0,0}),"camera right axis broken");
  Timeline timeline;require(timeline.push(p,500000),"initial state rejected");
  require(!timeline.push(p,600000),"duplicate refreshed stream timeout");
  require(!timeline.sample(1000001,30000,500000),"stale state held indefinitely");
  auto b=state(2,200000);require(timeline.push(b,600000),"next packet rejected");
  require(!timeline.push(state(1,250000),610000),"out of order packet accepted");
  auto mid=timeline.sample(600000,50000);
  require(mid && std::abs(mid->car.position.x-150)<0.01f,"sender-clock interpolation broken");
  require(timeline.push(state(1,20,8),700000),"sender restart rejected");
  require(!timeline.push(state(3,300000,7),710000),"retired session returned");
  require(timeline.sample(700000)->header.session==8,"session restart retained old pose");
  // Real loopback socket verifies the shared transport without either game.
  Udp receiver,sender;receiver.open(39741);sender.open(0);
  require(sender.send(&p,sizeof p,39741),"UDP send failed");
  char data[1024];std::uint16_t source{};int n=-1;
  const auto deadline=now_us()+1000000;
  while (n<0 && now_us()<deadline) n=receiver.receive(data,sizeof data,source);
  require(n==sizeof p && decode(data,n,decoded),"UDP loopback lost or corrupted state");
  if (argc>1) { std::ofstream f(argv[1],std::ios::binary);f.write(reinterpret_cast<const char*>(&p),sizeof p); }
  std::cout<<"Protocol, transforms, interpolation, timeout, restart and UDP passed\n";
}
