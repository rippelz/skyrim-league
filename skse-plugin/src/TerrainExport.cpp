#include "PCH.hpp"
#include "TerrainExport.hpp"
#include "shared/terrain_mesh.hpp"
#include "shared/dynamic_contacts.hpp"
#include "shared/actor_impact.hpp"
#pragma push_macro("InterlockedCompareExchange")
#undef InterlockedCompareExchange
namespace RE {class hkpRigidBody;}
#include <RE/B/bhkWorld.h>
#include <RE/B/bhkNiCollisionObject.h>
#include <RE/B/bhkRigidBody.h>
#include <RE/B/bhkCharacterController.h>
#include <RE/H/hkpWorld.h>
#include <RE/H/hkpSimulationIsland.h>
#include <RE/H/hkpEntity.h>
#include <RE/H/hkpRigidBody.h>
#include <RE/H/hkpMotion.h>
#include <RE/H/hkRefPtr.h>
#include <RE/H/hkpBvTreeShape.h>
#include <RE/H/hkpShapeContainer.h>
#include <RE/H/hkpConvexVerticesShape.h>
#include <RE/H/hkpShapeBuffer.h>
#include <RE/T/TESObjectCELL.h>
#pragma pop_macro("InterlockedCompareExchange")
#include <fstream>
#include <array>
#include <algorithm>
#include <cfloat>
#include <unordered_map>
#include <unordered_set>

namespace terrain_export {
namespace {
using bridge::Vec3;
Vec3 terrain_cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float terrain_dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 point(const float* xf,Vec3 p){return {xf[0]*p.x+xf[4]*p.y+xf[8]*p.z+xf[12],xf[1]*p.x+xf[5]*p.y+xf[9]*p.z+xf[13],xf[2]*p.x+xf[6]*p.y+xf[10]*p.z+xf[14]};}
Vec3 readvec(const void* p,std::size_t offset){Vec3 v{};std::memcpy(&v,static_cast<const char*>(p)+offset,sizeof v);return v;}
template<class T>T field(const void* p,std::size_t offset){T v{};std::memcpy(&v,static_cast<const char*>(p)+offset,sizeof v);return v;}
bool included(RE::COL_LAYER layer){switch(layer){case RE::COL_LAYER::kStatic:case RE::COL_LAYER::kAnimStatic:case RE::COL_LAYER::kTerrain:case RE::COL_LAYER::kGround:case RE::COL_LAYER::kTrees:case RE::COL_LAYER::kProps:case RE::COL_LAYER::kInvisibleWall:return true;default:return false;}}
struct Collector {
 const bridge::Transform& transform;
 float scale;
 Vec3 center;
 std::vector<bridge::TerrainTriangle> triangles;
 unsigned unsupported{};
 Vec3 rl(Vec3 p){return transform.inverse_position(p*(1/scale));}
 void emit(const float* xf,Vec3 a,Vec3 b,Vec3 c){
  bridge::TerrainTriangle t{rl(point(xf,a)),rl(point(xf,c)),rl(point(xf,b))}; // calibration reflects axes
  if(bridge::valid(t) && triangles.size()<bridge::kTerrainLimit)triangles.push_back(t);
 }
 bool overlaps(const RE::hkpShape* shape,const float* xf){
  RE::hkAabb box;shape->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(xf),0,box);
  float lo[4],hi[4];_mm_storeu_ps(lo,box.min.quad);_mm_storeu_ps(hi,box.max.quad);
  const float radius=5000*scale;
  return lo[0]<=center.x+radius && hi[0]>=center.x-radius && lo[1]<=center.y+radius && hi[1]>=center.y-radius && lo[2]<=center.z+radius && hi[2]>=center.z-radius;
 }
 void collect(const RE::hkpShape* shape,const float* xf,int depth=0){
  if(!shape || depth>12 || triangles.size()>=bridge::kTerrainLimit)return;
  using T=RE::hkpShapeType;auto type=shape->type;
  if(type==T::kTriangle){emit(xf,readvec(shape,0x30),readvec(shape,0x40),readvec(shape,0x50));return;}
  if(type==T::kMOPP || type==T::kBVTree){
   auto* bv=static_cast<const RE::hkpBvTreeShape*>(shape);auto* container=bv->GetContainer();if(!container)return;
   Vec3 lo{FLT_MAX,FLT_MAX,FLT_MAX},hi{-FLT_MAX,-FLT_MAX,-FLT_MAX};const float r=5000*scale;
   for(int i=0;i<8;++i){auto d=center+Vec3{(i&1)?r:-r,(i&2)?r:-r,(i&4)?r:-r}-Vec3{xf[12],xf[13],xf[14]};Vec3 p{xf[0]*d.x+xf[1]*d.y+xf[2]*d.z,xf[4]*d.x+xf[5]*d.y+xf[6]*d.z,xf[8]*d.x+xf[9]*d.y+xf[10]*d.z};lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};}
   RE::hkAabb local;local.min=RE::hkVector4(lo.x,lo.y,lo.z,0);local.max=RE::hkVector4(hi.x,hi.y,hi.z,0);
   std::vector<RE::hkpShapeKey> keys(65536);const auto found=std::min<std::uint32_t>(bv->QueryAabbImpl(local,keys.data(),keys.size()),keys.size());
   for(unsigned i=0;i<found;++i){RE::hkpShapeBuffer buffer;collect(container->GetChildShape(keys[i],buffer),xf,depth+1);}return;
  }
  if(type==T::kList || type==T::kCollection || type==T::kCompressedMesh || type==T::kExtendedMesh || type==T::kTriangleCollection || type==T::kConvexList){
   auto* container=shape->GetContainer();if(!container){++unsupported;return;}
   int guard=0;for(auto key=container->GetFirstKey();key!=RE::HK_INVALID_SHAPE_KEY && guard++<250000;key=container->GetNextKey(key)){RE::hkpShapeBuffer buffer;auto* child=container->GetChildShape(key,buffer);if(child && overlaps(child,xf))collect(child,xf,depth+1);}return;
  }
  if(type==T::kTransform || type==T::kConvexTransform || type==T::kConvexTranslate){
   auto* child=field<const RE::hkpShape*>(shape,type==T::kTransform?0x28:0x30);alignas(16) float local[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
   if(type==T::kConvexTranslate)std::memcpy(local+12,static_cast<const char*>(static_cast<const void*>(shape))+0x40,12);
   else std::memcpy(local,static_cast<const char*>(static_cast<const void*>(shape))+(type==T::kTransform?0x50:0x40),64);
   alignas(16) float combined[16]{};
   for(int i=0;i<4;++i){Vec3 v{local[i*4],local[i*4+1],local[i*4+2]};auto out=point(xf,v);if(i<3)out=out-Vec3{xf[12],xf[13],xf[14]};combined[i*4]=out.x;combined[i*4+1]=out.y;combined[i*4+2]=out.z;}combined[15]=1;collect(child,combined,depth+1);return;
  }
  if(type==T::kBox){
   auto half=readvec(shape,0x30);const float skin=field<float>(shape,0x20);half=half+Vec3{skin,skin,skin};Vec3 p[8];for(int i=0;i<8;++i)p[i]={(i&1)?half.x:-half.x,(i&2)?half.y:-half.y,(i&4)?half.z:-half.z};
   const int faces[][4]={{0,4,6,2},{1,3,7,5},{0,1,5,4},{2,6,7,3},{0,2,3,1},{4,5,7,6}};for(auto& f:faces){emit(xf,p[f[0]],p[f[1]],p[f[2]]);emit(xf,p[f[0]],p[f[2]],p[f[3]]);}return;
  }
  if(type==T::kConvexVertices){
   auto* convex=static_cast<const RE::hkpConvexVerticesShape*>(shape);if(convex->numVertices<4 || convex->numVertices>4096 || convex->planeEquations.size()>256){++unsupported;return;}
   std::vector<Vec3> vertices;for(int i=0;i<convex->rotatedVertices.size();++i){float x[4],y[4],z[4];auto& v=convex->rotatedVertices.data()[i];_mm_storeu_ps(x,v.x.quad);_mm_storeu_ps(y,v.y.quad);_mm_storeu_ps(z,v.z.quad);for(int j=0;j<4 && vertices.size()<static_cast<unsigned>(convex->numVertices);++j)vertices.push_back({x[j],y[j],z[j]});}
   for(int i=0;i<convex->planeEquations.size();++i){float plane[4];_mm_storeu_ps(plane,convex->planeEquations.data()[i].quad);Vec3 normal{plane[0],plane[1],plane[2]};std::vector<Vec3> face;Vec3 c{};
    for(auto p:vertices)if(std::abs(terrain_dot(normal,p)+plane[3])<0.005f){face.push_back(p+normal*field<float>(shape,0x20));c=c+face.back();}
    if(face.size()<3)continue;c=c*(1.0f/face.size());auto u=face[0]-c;u=u*(1/bridge::length(u));auto v=terrain_cross(normal,u);std::sort(face.begin(),face.end(),[&](Vec3 a,Vec3 b){a=a-c;b=b-c;return std::atan2(terrain_dot(a,v),terrain_dot(a,u))<std::atan2(terrain_dot(b,v),terrain_dot(b,u));});for(unsigned j=1;j+1<face.size();++j)emit(xf,face[0],face[j],face[j+1]);
   }return;
  }
  if(type==T::kSphere || type==T::kCapsule){
   const auto radius=field<float>(shape,0x20);const Vec3 a=type==T::kCapsule?readvec(shape,0x30):Vec3{},b=type==T::kCapsule?readvec(shape,0x40):Vec3{};
   auto axis=b-a;if(bridge::length(axis)<0.001f)axis={0,0,1};else axis=axis*(1/bridge::length(axis));auto u=terrain_cross(axis,std::abs(axis.z)<0.9f?Vec3{0,0,1}:Vec3{1,0,0});u=u*(1/bridge::length(u));auto v=terrain_cross(axis,u);
   auto vertex=[&](int lat,int lon){float phi=bridge::kPi*lat/12,theta=2*bridge::kPi*lon/24;auto center=lat<=6?b:a;return center+(u*(std::sin(phi)*std::cos(theta))+v*(std::sin(phi)*std::sin(theta))+axis*std::cos(phi))*radius;};
   for(int lat=0;lat<12;++lat)for(int lon=0;lon<24;++lon){auto p=vertex(lat,lon),q=vertex(lat,lon+1),r=vertex(lat+1,lon+1),s=vertex(lat+1,lon);emit(xf,p,s,r);emit(xf,p,r,q);}return;
  }
  ++unsupported;
 }
};
}
namespace {
struct LiveBody {RE::hkRefPtr<RE::hkpRigidBody> body;RE::ActorHandle actor;std::uint32_t uid{};std::uint64_t token{},shape{},seen{};const RE::hkpShape* native_shape{};std::vector<Vec3> points;};
std::unordered_map<RE::hkpEntity*,LiveBody> live_bodies;
std::unordered_map<std::uint32_t,LiveBody> characters;
std::unordered_map<std::uint32_t,std::uint64_t> damages;
struct Launch {RE::ActorHandle actor;Vec3 velocity;std::uint64_t ready{},expires{};};
std::vector<Launch> launches;
std::uint64_t live_session{},token_serial{},ack{},export_time{},dynamic_log{};
RE::hkpWorld* live_world{};
Vec3 hkvec(const RE::hkVector4& v){alignas(16) float p[4];_mm_store_ps(p,v.quad);return {p[0],p[1],p[2]};}
bridge::Quat orientation(const float* m){
 float x{},y{},z{},w{};const float trace=m[0]+m[5]+m[10];
 if(trace>0){const auto s=std::sqrt(trace+1)*2;w=.25f*s;x=(m[6]-m[9])/s;y=(m[8]-m[2])/s;z=(m[1]-m[4])/s;}
 else if(m[0]>m[5] && m[0]>m[10]){const auto s=std::sqrt(1+m[0]-m[5]-m[10])*2;w=(m[6]-m[9])/s;x=.25f*s;y=(m[4]+m[1])/s;z=(m[8]+m[2])/s;}
 else if(m[5]>m[10]){const auto s=std::sqrt(1+m[5]-m[0]-m[10])*2;w=(m[8]-m[2])/s;x=(m[4]+m[1])/s;y=.25f*s;z=(m[9]+m[6])/s;}
 else {const auto s=std::sqrt(1+m[10]-m[0]-m[5])*2;w=(m[1]-m[4])/s;x=(m[8]+m[2])/s;y=(m[9]+m[6])/s;z=.25f*s;}
 return bridge::normalized({x,y,z,w});
}
bridge::Quat inverse_rotation(const bridge::Transform& t,bridge::Quat sky){
 auto yaw=t.yaw_quat();auto q=bridge::multiply({-yaw.x,-yaw.y,-yaw.z,yaw.w},sky);return bridge::normalized({-q.y,-q.x,-q.z,q.w});
}
bool movable_layer(RE::COL_LAYER layer){switch(layer){
 case RE::COL_LAYER::kClutter:case RE::COL_LAYER::kWeapon:case RE::COL_LAYER::kProps:
 case RE::COL_LAYER::kDebrisSmall:case RE::COL_LAYER::kDebrisLarge:case RE::COL_LAYER::kDeadBip:
 case RE::COL_LAYER::kBiped:case RE::COL_LAYER::kBipedNoCC:case RE::COL_LAYER::kAnimStatic:case RE::COL_LAYER::kTrap:return true;
 default:return false;
}}
bool launch_node(RE::NiAVObject* node,Vec3 velocity){
 if(!node)return false;bool applied=false;
 if(auto* c=node->collisionObject?node->collisionObject->AsBhkNiCollisionObject():nullptr)if(auto* b=c->body?c->body->AsBhkRigidBody():nullptr){const auto scale=RE::bhkWorld::GetWorldScale();b->SetLinearVelocity(RE::hkVector4(velocity.x*scale,velocity.y*scale,velocity.z*scale,0));applied=true;}
 if(auto* parent=node->AsNode())for(const auto& child:parent->GetChildren())if(child)applied=launch_node(child.get(),velocity)||applied;
 return applied;
}
// hkpMaxSizeMotion is storage declared as a keyframed subclass, but contains
// the actual dynamic motion vtable. Dispatch through its abstract base without
// allowing the compiler to devirtualize to the storage class's no-op method.
__declspec(noinline) void apply_point(RE::hkpMotion& motion,const RE::hkVector4& impulse,const RE::hkVector4& point){motion.ApplyPointImpulse(impulse,point);}
void character_hit(RE::Actor* actor,const bridge::DynamicContact& c,const bridge::Transform& t,std::uint64_t now,float damage_scale,unsigned cooldown,bool character){
 if(!actor || actor->IsPlayerRef() || actor->IsDead())return;
 if(character && !actor->IsInRagdollState())if(auto* controller=actor->GetCharController()){
  RE::hkVector4 current;controller->GetLinearVelocityImpl(current);
  const auto delta=t.velocity(c.impulse)*(RE::bhkWorld::GetWorldScale()/80.f);
  const auto velocity=hkvec(current)+delta;
  if(bridge::valid(velocity))controller->SetLinearVelocityImpl(RE::hkVector4(velocity.x,velocity.y,velocity.z,0));
 }
 if(now-damages[actor->GetFormID()]<cooldown)return;
 const auto impact=bridge::actor_impact(c.closing,c.source?30.f:180.f,damage_scale);if(impact.damage<=0)return;
 damages[actor->GetFormID()]=now;actor->HandleHealthDamage(RE::PlayerCharacter::GetSingleton(),impact.damage);actor->SetBeenAttacked(true);
 if(!character)return; // Existing rigid ragdolls already received the native point impulse.
 const auto delta=t.velocity(c.impulse)*(1/80.f);const auto speed=bridge::length(delta);if(speed<1)return;
 auto point=t.position(c.point);auto direction=delta*(1/speed);
 if(auto* process=actor->GetActorRuntimeData().currentProcess)process->KnockExplosion(actor,{point.x-direction.x*100,point.y-direction.y*100,point.z-direction.z*100},std::clamp(speed/100,2.f,25.f));
 launches.push_back({actor->GetHandle(),delta,now+50000,now+750000});
 spdlog::info("Dynamic contact {} NPC {:08X} closing {} damage {}",c.source?"ball":"car",actor->GetFormID(),c.closing,impact.damage);
}
}
bool dynamic(RE::PlayerCharacter* player,const bridge::Transform& transform,std::uint64_t session,std::uint64_t now,const std::filesystem::path& terrain,float damage_scale,unsigned cooldown){
 auto* cell=player->GetParentCell();auto* world=cell?cell->GetbhkWorld():nullptr;auto* hk=world?world->GetWorld1():nullptr;if(!hk)return false;
 if(live_session!=session || live_world!=hk){live_bodies.clear();characters.clear();damages.clear();launches.clear();live_session=session;live_world=hk;ack=0;export_time=0;}
 for(auto it=launches.begin();it!=launches.end();){auto actor=it->actor.get();if(!actor || now>it->expires){it=launches.erase(it);continue;}if(now>=it->ready && actor->IsInRagdollState() && launch_node(actor->Get3D(),it->velocity))it=launches.erase(it);else ++it;}
 auto responses=terrain;responses.replace_extension(".rlcontacts");
 std::vector<bridge::DynamicContact> contacts;
 bridge::ContactHeader response{};std::ifstream incoming(responses,std::ios::binary);incoming.read(reinterpret_cast<char*>(&response),sizeof response);
 if(incoming && bridge::valid(response) && response.session==session && response.generation>ack && response.snapshot<=export_time && export_time-response.snapshot<500000){
  contacts.resize(response.count);incoming.read(reinterpret_cast<char*>(contacts.data()),contacts.size()*sizeof(bridge::DynamicContact));
  bool good=static_cast<bool>(incoming) && incoming.peek()==EOF;for(const auto& c:contacts)good&=bridge::valid(c);
  if(good){
   // Tokens must still resolve to a body in this exact Havok world. Never use an
   // address or form ID supplied by the response file as a native object pointer.
   std::unordered_map<std::uint64_t,LiveBody*> tokens;for(auto& [key,b]:live_bodies)tokens[b.token]=&b;for(auto& [key,b]:characters)tokens[b.token]=&b;
   struct ActorHit {RE::ActorHandle actor;bridge::DynamicContact contact;bool character;};
   std::vector<ActorHit> actor_hits;
   {
    RE::BSWriteLockGuard lock(world->worldLock);
    for(const auto& c:contacts){auto found=tokens.find(c.id);if(found==tokens.end())continue;auto* b=found->second;
     if(b->body && b->body->world==hk && b->body->uid==b->uid && b->body->motion.type!=RE::hkpMotion::MotionType::kFixed && b->body->motion.type!=RE::hkpMotion::MotionType::kKeyframed){
      const auto scale=RE::bhkWorld::GetWorldScale();const auto impulse=transform.velocity(c.impulse)*scale;const auto point=transform.position(c.point)*scale;
      // The public body operation wakes the island. The motion's native point
      // impulse then supplies linear AND angular response at the contact point.
      b->body->ApplyLinearImpulse(RE::hkVector4(0,0,0,0));
      apply_point(static_cast<RE::hkpMotion&>(b->body->motion),RE::hkVector4(impulse.x,impulse.y,impulse.z,0),RE::hkVector4(point.x,point.y,point.z,0));
     }
     if(b->actor)actor_hits.push_back({b->actor,c,!b->body});
    }
   }
   std::unordered_map<std::uint32_t,ActorHit> aggregate;
   for(auto& hit:actor_hits)if(auto actor=hit.actor.get()){
    auto [it,inserted]=aggregate.try_emplace(actor->GetFormID(),hit);
    if(!inserted){it->second.contact.impulse=it->second.contact.impulse+hit.contact.impulse;it->second.contact.closing=std::max(it->second.contact.closing,hit.contact.closing);it->second.character|=hit.character;}
   }
   for(auto& [id,hit]:aggregate)if(auto actor=hit.actor.get())character_hit(actor.get(),hit.contact,transform,now,damage_scale,cooldown,hit.character);
   ack=response.generation;
  }
 }
 if(now-export_time<33000)return true;
 const auto scale=RE::bhkWorld::GetWorldScale();const auto pos=player->GetPosition();const Vec3 center{pos.x*scale,pos.y*scale,pos.z*scale};
 std::vector<bridge::DynamicBody> records;std::vector<Vec3> vertices;
 {
  RE::BSReadLockGuard lock(world->worldLock);
  auto gather=[&](RE::hkpSimulationIsland* island){if(!island)return;
   for(int i=0;i<island->entities.size() && records.size()<bridge::kDynamicLimit;++i){auto* entity=island->entities.data()[i];if(!entity || entity->motion.type==RE::hkpMotion::MotionType::kFixed || !movable_layer(entity->collidable.GetCollisionLayer()))continue;
    auto& col=entity->collidable;auto* xf=static_cast<const float*>(col.motion);if(!col.shape || !xf || entity->material.GetResponseType()!=RE::hkpMaterial::ResponseType::kSimpleContact)continue;
    if(auto* ref=entity->GetUserData())if(auto* actor=ref->As<RE::Actor>())if(actor->IsPlayerRef())continue;
    bool finite=true;for(int j=0;j<16;++j)finite&=std::isfinite(xf[j]) && std::abs(xf[j])<1e7f;if(!finite)continue;
    const Vec3 origin{xf[12],xf[13],xf[14]};if(bridge::length(origin-center)>1800*scale)continue;
    auto& live=live_bodies[entity];if(!live.body || live.uid!=entity->uid){live={};live.body=RE::hkRefPtr<RE::hkpRigidBody>(static_cast<RE::hkpRigidBody*>(entity));live.uid=entity->uid;live.token=++token_serial;}
    live.seen=now;
    if(auto* ref=entity->GetUserData())if(auto* actor=ref->As<RE::Actor>())if(!actor->IsPlayerRef())live.actor=actor->GetHandle();
    bridge::DynamicBody record;record.id=live.token;if(live.native_shape!=col.shape){live.native_shape=col.shape;live.shape=++token_serial;live.points.clear();}record.shape=live.shape;
    record.state.position=transform.inverse_position(origin*(1/scale));record.state.rotation=inverse_rotation(transform,orientation(xf));
    record.state.velocity=transform.inverse_direction(hkvec(entity->motion.linearVelocity))*(1/(scale*transform.scale));
    record.state.angular_velocity=transform.inverse_direction(hkvec(entity->motion.angularVelocity))*(-1);
    record.mass=entity->motion.type==RE::hkpMotion::MotionType::kKeyframed?0:entity->motion.GetMass();
    record.friction=std::clamp(entity->material.friction,0.f,10.f);record.restitution=std::clamp(entity->material.restitution,0.f,1.f);
    if(live.points.empty()){
     Collector geometry{transform,scale,origin};geometry.collect(col.shape,xf);
     std::unordered_set<std::string> unique;const auto q=record.state.rotation;const bridge::Quat inverse{-q.x,-q.y,-q.z,q.w};
     for(const auto& tri:geometry.triangles)for(auto p:{tri.a,tri.b,tri.c}){
      p=bridge::rotate(inverse,p-record.state.position);auto key=std::to_string(std::lround(p.x*100))+","+std::to_string(std::lround(p.y*100))+","+std::to_string(std::lround(p.z*100));if(unique.insert(key).second)live.points.push_back(p);
     }
    }
    const auto& points=live.points;
    if(points.size()<4 || points.size()>4096 || vertices.size()+points.size()>bridge::kDynamicVertexLimit)continue;
    record.first=vertices.size();record.count=points.size();if(!bridge::valid(record,vertices.size()+points.size()))continue;
    vertices.insert(vertices.end(),points.begin(),points.end());records.push_back(record);
   }
  };
  gather(hk->fixedIsland);for(int i=0;i<hk->activeSimulationIslands.size();++i)gather(hk->activeSimulationIslands.data()[i]);for(int i=0;i<hk->inactiveSimulationIslands.size();++i)gather(hk->inactiveSimulationIslands.data()[i]);
 }
 // Standing NPCs are character controllers, not movable rigid bodies. Once the
 // actor enters ragdoll, its actual Havok bone bodies replace this capsule hull.
 if(auto* processes=RE::ProcessLists::GetSingleton())processes->ForEachHighActor([&](RE::Actor* actor){
  if(!actor || actor->IsPlayerRef() || actor->IsDead() || actor->IsInRagdollState() || !actor->Is3DLoaded() || records.size()>=bridge::kDynamicLimit)return RE::BSContainer::ForEachResult::kContinue;
  auto p=actor->GetPosition();Vec3 origin{p.x,p.y,p.z+45};if(bridge::length(origin-Vec3{pos.x,pos.y,pos.z})>1800)return RE::BSContainer::ForEachResult::kContinue;
  auto& live=characters[actor->GetFormID()];if(!live.token){live.token=++token_serial;live.actor=actor->GetHandle();}live.seen=now;
  bridge::DynamicBody record;record.id=live.token;record.shape=live.token;record.flags=bridge::kCharacterBody;record.mass=80;record.state.position=transform.inverse_position(origin);
  RE::NiPoint3 velocity{};actor->GetLinearVelocity(velocity);record.state.velocity=transform.inverse_direction({velocity.x,velocity.y,velocity.z})*(1/transform.scale);
  record.first=vertices.size();
  for(int lat=0;lat<=8;++lat)for(int lon=0;lon<12;++lon){const float phi=bridge::kPi*lat/8,theta=2*bridge::kPi*lon/12;const auto z=(lat<=4?18.f:-18.f)+22*std::cos(phi);const Vec3 sky{22*std::sin(phi)*std::cos(theta),22*std::sin(phi)*std::sin(theta),z};vertices.push_back(transform.inverse_direction(sky)*(1/transform.scale));}
  record.count=vertices.size()-record.first;records.push_back(record);return RE::BSContainer::ForEachResult::kContinue;
 });
 for(auto it=live_bodies.begin();it!=live_bodies.end();)if(it->second.seen!=now)it=live_bodies.erase(it);else ++it;
 for(auto it=characters.begin();it!=characters.end();)if(it->second.seen!=now)it=characters.erase(it);else ++it;
 bridge::DynamicHeader header;header.session=session;header.generation=now;header.ack=ack;header.bodies=records.size();header.vertices=vertices.size();
 auto path=terrain;path.replace_extension(".rldyn");auto tmp=path;tmp+=".tmp";
 std::ofstream output(tmp,std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char*>(&header),sizeof header);output.write(reinterpret_cast<const char*>(records.data()),records.size()*sizeof(bridge::DynamicBody));output.write(reinterpret_cast<const char*>(vertices.data()),vertices.size()*sizeof(Vec3));output.close();
 if(!output || !MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))return false;export_time=now;
 if(now-dynamic_log>1000000){dynamic_log=now;spdlog::info("Dynamic collision snapshot: {} rigid bodies, {} character controllers, {} vertices, acknowledged batch {}",live_bodies.size(),characters.size(),vertices.size(),ack);}
 return true;
}

bool write(RE::PlayerCharacter* player,const bridge::Transform& transform,std::uint64_t session,std::uint64_t generation,const std::filesystem::path& path){
 auto* cell=player->GetParentCell();auto* world=cell?cell->GetbhkWorld():nullptr;if(!world)return false;auto* hk=world->GetWorld1();if(!hk)return false;
 auto pos=player->GetPosition();const float scale=RE::bhkWorld::GetWorldScale();Collector out{transform,scale,{pos.x*scale,pos.y*scale,pos.z*scale}};
 {
  RE::BSReadLockGuard lock(world->worldLock);
  auto gather=[&](RE::hkpSimulationIsland* island){if(!island)return;for(int i=0;i<island->entities.size();++i){auto* entity=island->entities.data()[i];if(!entity || entity->motion.type!=RE::hkpMotion::MotionType::kFixed)continue;auto& col=entity->collidable;auto* xf=static_cast<const float*>(col.motion);if(!col.shape||!xf||!included(col.GetCollisionLayer()))continue;bool finite=true;for(int j=0;j<16;++j)finite&=std::isfinite(xf[j]) && std::abs(xf[j])<1e7f;if(finite && out.overlaps(col.shape,xf))out.collect(col.shape,xf);}};
  gather(hk->fixedIsland);for(int i=0;i<hk->activeSimulationIslands.size();++i)gather(hk->activeSimulationIslands.data()[i]);for(int i=0;i<hk->inactiveSimulationIslands.size();++i)gather(hk->inactiveSimulationIslands.data()[i]);
 }
 if(out.triangles.empty() || out.triangles.size()>=bridge::kTerrainLimit){spdlog::warn("Native terrain export rejected: triangle count {} unsupported {}",out.triangles.size(),out.unsupported);return false;}
 bridge::TerrainHeader header;header.session=session;header.generation=generation;header.triangles=out.triangles.size();auto temporary=path;temporary+=".tmp";
 std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(&header),sizeof header);file.write(reinterpret_cast<const char*>(out.triangles.data()),out.triangles.size()*sizeof(bridge::TerrainTriangle));file.close();if(!file)return false;
 if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
 spdlog::info("Native terrain exported {} real Havok triangles; unsupported shapes {} generation {}",header.triangles,out.unsupported,generation);return true;
}
}
