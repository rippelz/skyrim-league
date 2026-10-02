#include "DirectRender.hpp"
#include "CarMenu.hpp"
#include "TerrainExport.hpp"
#include "RE/B/BSFadeNode.h"
#include "RE/B/BSTriShape.h"
#include "PCH.hpp"
#include "shared/timeline.hpp"
#include "shared/packet_receiver.hpp"
#include "shared/actor_impact.hpp"
#include <RE/B/bhkNiCollisionObject.h>
#include <RE/B/bhkRigidBody.h>
#include <cctype>
#include <RE/B/BSResourceNiBinaryStream.h>
#pragma push_macro("InterlockedCompareExchange")
#undef InterlockedCompareExchange
#include <RE/B/BSMultiBoundNode.h>
#pragma pop_macro("InterlockedCompareExchange")
#include <RE/M/Main.h>
#include <RE/M/MenuControls.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <unordered_set>
#include <RE/N/NiCamera.h>
#include <RE/S/Sky.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESGlobal.h>
#include <RE/T/TESObjectCELL.h>
#include <unordered_map>
#include <RE/B/BSVisit.h>
#include <RE/B/BSGeometry.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/bhkWorld.h>
#include <RE/B/bhkPickData.h>
#include <RE/H/hkpCollidable.h>
namespace RE { class hkpRigidBody; }
#include <RE/H/hkpWorld.h>

namespace {
using namespace bridge;
Vec3 vec(const RE::NiPoint3& p) { return {p.x,p.y,p.z}; }
RE::NiPoint3 vec(Vec3 p) { return {p.x,p.y,p.z}; }
RE::NiMatrix3 mat(Quat q) {
  const auto m=matrix(q);RE::NiMatrix3 out;
  for (int i=0;i<3;++i) for (int j=0;j<3;++j) out.entry[i][j]=m[i][j];
  return out;
}
struct Config {
  int state_port=29741,event_port=29742,toggle_key=66,anchor_key=67,ball_key=68;
  std::uint64_t delay=30000,timeout=500000,npc_cooldown=750000;
  float scale=1.0f/1.43f,yaw=0,car_scale=1,ball_scale=1,npc_radius=90,max_range=8192,car_clearance=17,npc_damage_scale=.03f;
  bool follow_camera=true,npc_bumps=true,auto_start=false,test_cell=false,suppress_survival_prompt=false,terrain_collisions=false;
  std::string car_model="Clutter\\Dwemer\\DwePuzzleCube.nif";
  std::string ball_model="Clutter\\Food\\CheeseWheel01A.nif";
  bool native_terrain=false;
  std::string terrain_mesh_path;
  void load() {
    wchar_t executable[32768]{};
    const auto length=GetModuleFileNameW(nullptr,executable,32768);
    if (!length || length>=32768) throw std::runtime_error("Could not find Skyrim executable directory");
    const auto path=(std::filesystem::path(executable).parent_path()/"Data/SKSE/Plugins/SkyrimRocketBridge.ini").string();
    auto str=[&](const char* key,const std::string& value) {
      char out[512]{};GetPrivateProfileStringA("Bridge",key,value.c_str(),out,sizeof out,path.c_str());return std::string(out);
    };
    auto integer=[&](const char* key,int v,int lo,int hi) {
      try { return std::clamp(std::stoi(str(key,std::to_string(v))),lo,hi); } catch (...) { return v; }
    };
    auto real=[&](const char* key,float v,float lo,float hi) {
      try { const auto n=std::stof(str(key,std::to_string(v)));return std::isfinite(n)?std::clamp(n,lo,hi):v; } catch (...) { return v; }
    };
    state_port=integer("StatePort",state_port,1024,65535);event_port=integer("EventPort",event_port,1024,65535);
    toggle_key=integer("ToggleKey",toggle_key,1,255);anchor_key=integer("AnchorKey",anchor_key,1,255);ball_key=integer("ResetBallKey",ball_key,1,255);
    delay=integer("InterpolationMs",30,0,200)*1000ull;timeout=integer("TimeoutMs",500,100,5000)*1000ull;
    scale=real("Scale",scale,0.01f,10);yaw=real("YawDegrees",0,-360,360)*kPi/180;
    car_scale=real("CarScale",car_scale,0.01f,100);ball_scale=real("BallScale",ball_scale,0.01f,100);
    car_clearance=real("AnchorCarClearance",car_clearance,0,100);
    npc_radius=real("NpcRadius",npc_radius,10,500);max_range=real("MaxRange",0,0,1e7f);
    npc_cooldown=integer("NpcCooldownMs",750,100,10000)*1000ull;
    npc_damage_scale=real("NpcDamageScale",.03f,0,.1f);
    follow_camera=integer("FollowCamera",1,0,1)!=0;npc_bumps=integer("NpcBumps",1,0,1)!=0;
    auto_start=integer("AutoStart",0,0,1)!=0;
    suppress_survival_prompt=integer("SuppressSurvivalPrompt",0,0,1)!=0;
    terrain_collisions=integer("TerrainCollisions",0,0,1)!=0;
    native_terrain=integer("NativeTerrain",0,0,1)!=0;
    terrain_mesh_path=str("TerrainMeshPath","");
    test_cell=str("TestCell","")=="qasmoke";
    car_model=str("CarModel",car_model);ball_model=str("BallModel",ball_model);
    spdlog::info("Config {}: auto-start {} test-cell {} car {} ball {}",path,auto_start,test_cell,car_model,ball_model);
  }
} cfg;
Udp socket;
PacketReceiver receiver(socket);
std::uint64_t receive_drops{},receive_gap{},predicted_frames{},prediction_max_age{};
Timeline timeline;
Transform transform;
StatePacket rendered;
bool active{},camera_ready{},auto_start_suppressed{};
RE::ObjectRefHandle conversation_target;
void send_event(EventType,Vec3,Vec3,Quat={});
std::uint64_t last_diagnostic{},last_auto_attempt{};
RE::NiPoint3 saved_position;
RE::NiPoint3 saved_angle;
bool saved_collision{},saved_first_person{},saved_vanity{};
float saved_fov{},saved_first_fov{};
RE::TESWorldSpace* saved_world{};
RE::TESObjectCELL* saved_cell{};
std::vector<RE::NiPointer<RE::BSGeometry>> hidden_meshes;
RE::NiPointer<RE::NiNode> car_node,ball_node;
std::uint32_t event_sequence{};
std::uint64_t camera_update_calls{};
std::uint64_t last_terrain_export{};
Vec3 last_terrain_position{};
std::unordered_map<RE::FormID,std::uint64_t> npc_hits;
struct PendingLaunch {RE::ActorHandle actor;Vec3 velocity;std::uint64_t ready,expires;};
std::vector<PendingLaunch> npc_launches;
StatePacket previous_impact{};bool have_previous_impact{},interaction_requested{},resume_after_world{},reanchor_after_loading{};
std::unordered_set<std::string> blocking_menus;
std::uint64_t menu_input_session{},menu_input_time{};


bool native_menu() {
  auto* ui=RE::UI::GetSingleton();
  return !ui || !blocking_menus.empty() || ui->IsApplicationMenuOpen() || ui->IsItemMenuOpen() || ui->GameIsPaused() || ui->IsModalMenuOpen() || ui->IsMenuOpen("Console") ||
    ui->IsMenuOpen("Loading Menu") || ui->IsMenuOpen("Main Menu") || ui->IsMenuOpen("Dialogue Menu") ||
    ui->IsMenuOpen("FavoritesMenu") || ui->IsMenuOpen("InventoryMenu") || ui->IsMenuOpen("MagicMenu") ||
    ui->IsMenuOpen("MapMenu") || ui->IsMenuOpen("StatsMenu") || ui->IsMenuOpen("TweenMenu") ||
    ui->IsMenuOpen("Journal Menu") || ui->IsMenuOpen("Sleep/Wait Menu");
}
bool menu(){return car_menu::busy() || native_menu();}
void suppress_survival_prompt() {
  if (!cfg.suppress_survival_prompt) return;
  auto* data=RE::TESDataHandler::GetSingleton();
  auto* prompted=data?data->LookupForm<RE::TESGlobal>(0x8DF,"ccQDRSSE001-SurvivalMode.esl"):nullptr;
  if (prompted) {
    prompted->value=1.0f;
    spdlog::info("Survival startup prompt suppressed at user request");
  }
}
void detach(RE::NiPointer<RE::NiNode>& node) {
  if (node && node->parent) node->parent->DetachChild(node.get());
  node.reset();
}
void restore_hidden() {
  for (auto& mesh:hidden_meshes) if (mesh) mesh->SetAppCulled(false);
  hidden_meshes.clear();
}
void hide_player(RE::PlayerCharacter* player) {
  if (!player) return;
  // Hide leaf geometry, keeping skeleton nodes available to animation/camera.
  // Adapted from SkyCraft's mesh hiding (MIT; see THIRD_PARTY.md).
  for (bool first_person:{false,true}) if (auto* root=player->Get3D(first_person)) {
    RE::BSVisit::TraverseScenegraphGeometries(root,[&](RE::BSGeometry* mesh) {
      if (!mesh->GetAppCulled()) {
        if (std::none_of(hidden_meshes.begin(),hidden_meshes.end(),[&](const auto& p){return p.get()==mesh;})) hidden_meshes.emplace_back(mesh);
        mesh->SetAppCulled(true);
      }
      return RE::BSVisit::BSVisitControl::kContinue;
    });
  }
}
void stop(bool restore_position=true) {
  car_menu::close();
  if (!active) { camera_ready=false;detach(car_node);detach(ball_node);return; }
  send_event(EventType::PauseDriving,{},{});conversation_target={};
  active=false;camera_ready=false;direct_render::publish(false,{},{},false,false);
  detach(car_node);detach(ball_node);restore_hidden();npc_hits.clear();npc_launches.clear();have_previous_impact=false;
  auto* player=RE::PlayerCharacter::GetSingleton();
  if (player) {
    player->SetCollision(saved_collision);
    if (restore_position && player->GetWorldspace()==saved_world &&
        (saved_world || player->GetParentCell()==saved_cell)) {
      player->SetPosition(saved_position,true);player->SetAngle(saved_angle);
    }
  }
  if (auto* cam=RE::PlayerCamera::GetSingleton()) {
    cam->GetRuntimeData2().worldFOV=saved_fov;cam->GetRuntimeData2().firstPersonFOV=saved_first_fov;cam->GetRuntimeData2().allowAutoVanityMode=saved_vanity;
    if (saved_first_person) cam->ForceFirstPerson();else cam->ForceThirdPerson();
  }
  spdlog::info("Bridge stopped; restored player/camera");
}
void strip_collision(RE::NiAVObject* obj) {
  obj->collisionObject.reset();
  // Transient streamed visuals have no TES reference to drive fade/occlusion.
  obj->GetFlags().set(RE::NiAVObject::Flag::kAlwaysDraw,RE::NiAVObject::Flag::kIgnoreFade);
  if (auto* fade=obj->AsFadeNode()) fade->GetRuntimeData().currentFade=1;
  if (auto* node=obj->AsNode()) for (const auto& child:node->GetChildren()) if (child) strip_collision(child.get());
}
RE::NiPointer<RE::NiNode> model(const std::string& path) {
  RE::NiPointer<RE::NiNode> source;
  RE::BSModelDB::DBTraits::ArgsType args;
  // The resource database expects a Data-relative path, while model INI
  // entries follow TESModel's mesh-relative convention.
  auto resource=path;
  auto prefix=resource.substr(0,7);
  std::transform(prefix.begin(),prefix.end(),prefix.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (prefix!="meshes\\" && prefix!="meshes/") resource="meshes\\"+resource;
  const auto error=RE::BSModelDB::Demand(resource.c_str(),source,args);
  if (error!=RE::BSResource::ErrorCode::kNone || !source) {
    static bool diagnosed=false;
    if (!diagnosed) {
      diagnosed=true;
      RE::BSResourceNiBinaryStream stream(resource);
      RE::NiPointer<RE::NiNode> native;
      const auto native_error=RE::BSModelDB::Demand("meshes\\clutter\\food\\cheesewheel01a.nif",native,args);
      spdlog::error("Model resource diagnostics: disk attributes {} stream good {} native error {} native loaded {}",
        GetFileAttributesA(("Data\\"+resource).c_str()),stream.good(),static_cast<int>(native_error),static_cast<bool>(native));
    }
    spdlog::error("Model load failed: {} resource error {}",resource,static_cast<int>(error));return {};
  }
  auto* copy=source->Clone();
  RE::NiPointer<RE::NiNode> result{copy?copy->AsNode():nullptr};
  if (result) {
    strip_collision(result.get());
    RE::BSVisit::TraverseScenegraphGeometries(result.get(),[&](RE::BSGeometry* g) {
      auto* shader=g->GetGeometryRuntimeData().shaderProperty.get();
      if (shader) { shader->SetupGeometry(g);shader->FinishSetupGeometry(g); }
      if (auto* tri=g->AsTriShape()) spdlog::info("Model triangles {} count {} vertices {} scale {}",g->name.c_str(),tri->GetTrishapeRuntimeData().triangleCount,tri->GetTrishapeRuntimeData().vertexCount,g->local.scale);
      return RE::BSVisit::BSVisitControl::kContinue;
    });
  }
  return result;
}
void place(RE::NiNode* node,Vec3 position,Quat rotation,float scale) {
  if (!node) return;
  RE::NiTransform world;world.translate=vec(position);world.rotate=mat(rotation);world.scale=scale;
  node->local=node->parent?node->parent->world.Invert()*world:world;
  RE::NiUpdateData update{};update.flags=RE::NiUpdateData::Flag::kDirty;node->Update(update);
}
void anchor() {
  last_terrain_export=0;have_previous_impact=false;
  const auto p=timeline.sample(now_us(),0,cfg.timeout);
  auto* player=RE::PlayerCharacter::GetSingleton();
  if (!p || !(p->flags&CarValid) || !player) return;
  auto rl_origin=p->car.position;rl_origin.z-=cfg.car_clearance;
  transform={cfg.scale,cfg.yaw,rl_origin,vec(player->GetPosition())};
  spdlog::info("Anchor Skyrim ({}, {}, {}) RL ({}, {}, {}) scale {}",transform.sky_origin.x,
    transform.sky_origin.y,transform.sky_origin.z,transform.rl_origin.x,transform.rl_origin.y,transform.rl_origin.z,transform.scale);
}
void start() {
  if (menu()) return;
  cfg.load();
  const auto p=timeline.sample(now_us(),0,cfg.timeout);
  auto* player=RE::PlayerCharacter::GetSingleton();auto* cam=RE::PlayerCamera::GetSingleton();
  auto* tes=RE::TES::GetSingleton();
  if (!p || !(p->flags&CarValid) || !player || !cam || !player->Is3DLoaded() || !tes || !tes->objRoot) {
    RE::SendHUDMessage::ShowHUDMessage("Rocket bridge: no fresh car stream (start sb_start or the demo)");return;
  }
  car_node=model(cfg.car_model);ball_node=model(cfg.ball_model);
  if (!car_node || !ball_node) {
    detach(car_node);detach(ball_node);RE::SendHUDMessage::ShowHUDMessage("Rocket bridge: missing model (see log/INI)");return;
  }
  saved_position=player->GetPosition();saved_angle=player->GetAngle();saved_world=player->GetWorldspace();saved_cell=player->GetParentCell();
  saved_collision=player->HasCollision();saved_first_person=cam->IsInFirstPerson();
  saved_fov=cam->GetRuntimeData2().worldFOV;saved_first_fov=cam->GetRuntimeData2().firstPersonFOV;saved_vanity=cam->GetRuntimeData2().allowAutoVanityMode;
  anchor();tes->objRoot->AttachChild(car_node.get());tes->objRoot->AttachChild(ball_node.get());
  player->SetCollision(false);cam->GetRuntimeData2().allowAutoVanityMode=false;cam->ForceThirdPerson();
  active=true;reanchor_after_loading=false;RE::SendHUDMessage::ShowHUDMessage("Rocket bridge on; F8 exit, F9 anchor, F10 reset ball");
}
void send_event(EventType type,Vec3 pos,Vec3 velocity,Quat rotation) {
  const auto* last=timeline.latest();if (!last) return;
  EventPacket e;
  e.header={kMagic,kVersion,Kind::Event,sizeof e,++event_sequence,last->header.session,now_us()};
  e.target_sequence=last->header.sequence;e.type=type;e.position=pos;e.velocity=velocity;e.rotation=rotation;
  if (valid(e)) socket.send(&e,sizeof e,static_cast<std::uint16_t>(cfg.event_port));
}
// Query the loaded Skyrim Havok world, with engine-provided unit scale.
// Surface packets contain a plane point and unit normal in RL coordinates.
EventPacket wheel_probes[4]{};
std::uint64_t wheel_probe_times[4]{};
void wheel_surfaces(RE::PlayerCharacter* player) {
  auto* cell=player->GetParentCell();auto* world=cell?cell->GetbhkWorld():nullptr;
  if(!world || !timeline.latest())return;
  auto* native=world->GetWorld1();if(!native)return;
  const auto scale=RE::bhkWorld::GetWorldScale();
  for(int i=0;i<4;++i) {
    const auto& probe=wheel_probes[i];
    if(now_us()-wheel_probe_times[i]>100000 || probe.header.session!=timeline.latest()->header.session)continue;
    auto from=transform.position(probe.position),to=from+transform.velocity(probe.velocity);
    RE::bhkPickData pick{};bool hit=false;
    RE::BSReadLockGuard lock(world->worldLock);
    for(int attempt=0;attempt<8;++attempt) {
      pick.rayOutput.Reset();pick.rayInput.from=RE::hkVector4(vec(from*scale));pick.rayInput.to=RE::hkVector4(vec(to*scale));pick.rayInput.enableShapeCollectionFilter=false;pick.rayInput.filterInfo={};native->CastRay(pick.rayInput,pick.rayOutput);
      if(!pick.rayOutput.HasHit())break;
      const auto layer=pick.rayOutput.rootCollidable->GetCollisionLayer();
      if(layer==RE::COL_LAYER::kStatic || layer==RE::COL_LAYER::kAnimStatic || layer==RE::COL_LAYER::kTerrain || layer==RE::COL_LAYER::kGround || layer==RE::COL_LAYER::kProps || layer==RE::COL_LAYER::kStairHelper) {hit=true;break;}
      auto remaining=to-from;auto distance=length(remaining);if(distance<2)break;from=from+remaining*std::min(1.0f,pick.rayOutput.hitFraction+2/distance);
    }
    if(hit) {float n[4];_mm_storeu_ps(n,pick.rayOutput.normal.quad);send_event(static_cast<EventType>(7+i),transform.inverse_position(from+(to-from)*pick.rayOutput.hitFraction),transform.inverse_direction({n[0],n[1],n[2]}));}
    else send_event(static_cast<EventType>(11+i),{},{});
  }
}
void terrain_surfaces(RE::PlayerCharacter* player,const StatePacket& packet) {
  if (!cfg.terrain_collisions) return;
  auto* cell=player->GetParentCell();auto* world=cell?cell->GetbhkWorld():nullptr;
  if (!world) return;
  const float havok_scale=RE::bhkWorld::GetWorldScale();
  const auto query=[&](const Body& body,EventType type) {
    const auto center=transform.position(body.position);
    const auto cast=[&](Vec3 from,Vec3 to) {
      RE::bhkPickData pick{};
      auto* native_world=world->GetWorld1();if (!native_world) return;
      RE::BSReadLockGuard lock(world->worldLock);
      for (int attempt=0;attempt<8;++attempt) {
        pick.rayOutput.Reset();
        pick.rayInput.from=RE::hkVector4(vec(from*havok_scale));
        pick.rayInput.to=RE::hkVector4(vec(to*havok_scale));
        pick.rayInput.enableShapeCollectionFilter=false;
        pick.rayInput.filterInfo={};
        native_world->CastRay(pick.rayInput,pick.rayOutput);
        if (!pick.rayOutput.HasHit()) return;
        const auto layer=pick.rayOutput.rootCollidable->GetCollisionLayer();
        if (layer==RE::COL_LAYER::kStatic || layer==RE::COL_LAYER::kAnimStatic || layer==RE::COL_LAYER::kTerrain || layer==RE::COL_LAYER::kGround || layer==RE::COL_LAYER::kInvisibleWall || layer==RE::COL_LAYER::kTrees || layer==RE::COL_LAYER::kProps || layer==RE::COL_LAYER::kStairHelper) break;
        if (attempt==7) return;
        const auto remaining=to-from;const auto distance=length(remaining);
        if (distance<2) return;
        from=from+remaining*std::min(1.0f,pick.rayOutput.hitFraction+2.0f/distance);
      }
      const auto layer=pick.rayOutput.rootCollidable->GetCollisionLayer();
      const auto fraction=pick.rayOutput.hitFraction;
      if (!std::isfinite(fraction) || fraction<0 || fraction>1) return;
      float n[4];_mm_storeu_ps(n,pick.rayOutput.normal.quad);
      const auto normal=transform.inverse_direction({n[0],n[1],n[2]});
      if (length(normal)<0.9f || length(normal)>1.1f) return;
      static std::uint64_t diagnostic{};
      if (now_us()-diagnostic>1000000) {
        diagnostic=now_us();spdlog::info("Terrain hit layer {} fraction {} point ({}, {}, {}) normal ({}, {}, {}) havok scale {}",static_cast<int>(layer),fraction,
          (from+(to-from)*fraction).x,(from+(to-from)*fraction).y,(from+(to-from)*fraction).z,normal.x,normal.y,normal.z,havok_scale);
      }
      send_event(type,transform.inverse_position(from+(to-from)*fraction),normal);
    };
    // Vertical search recovers objects sunk under terrain by the original arena.
    cast(center+Vec3{0,0,512},center-Vec3{0,0,2048});
    // Forward/side probes detect walls and bridge parapets. These are discrete
    // probes, not a cooked replacement collision mesh (see README limitations).
    const float radius=type==EventType::BallSurface?94.0f:65.0f;
    for (Vec3 axis:{Vec3{1,0,0},Vec3{-1,0,0},Vec3{0,1,0},Vec3{0,-1,0}}) {
      const auto dir=transform.direction(axis);
      cast(center,center+dir*((radius+length(body.velocity)/60.0f)*transform.scale));
    }
  };
  query(packet.car,EventType::CarSurface);
  if (packet.flags&BallValid) query(packet.ball,EventType::BallSurface);
}
bool launch_bones(RE::NiAVObject* node,Vec3 velocity) {
  if(!node)return false;bool applied=false;
  if(auto* collision=node->collisionObject?node->collisionObject->AsBhkNiCollisionObject():nullptr) {
    if(auto* body=collision->body?collision->body->AsBhkRigidBody():nullptr) {
      const auto scale=RE::bhkWorld::GetWorldScale();
      body->SetLinearVelocity(RE::hkVector4(velocity.x*scale,velocity.y*scale,velocity.z*scale,0));applied=true;
    }
  }
  if(auto* parent=node->AsNode())for(const auto& child:parent->GetChildren())if(child)applied=launch_bones(child.get(),velocity)||applied;
  return applied;
}
void npc_bumps(Vec3,std::uint64_t now) {
  for(auto it=npc_launches.begin();it!=npc_launches.end();) {
    auto actor=it->actor.get();
    if(!actor || now>it->expires){it=npc_launches.erase(it);continue;}
    if(now>=it->ready && actor->IsInRagdollState() && launch_bones(actor->Get3D(),it->velocity)) {
      spdlog::info("NPC native ragdoll launch {:08X} speed {}",actor->GetFormID(),length(it->velocity));it=npc_launches.erase(it);
    }else ++it;
  }
  if(!cfg.npc_bumps)return;
  auto* processes=RE::ProcessLists::GetSingleton();if(!processes)return;
  const bool continuous=have_previous_impact && previous_impact.header.session==rendered.header.session &&
    rendered.header.timestamp_us>=previous_impact.header.timestamp_us && rendered.header.timestamp_us-previous_impact.header.timestamp_us<200000;
  processes->ForEachHighActor([&](RE::Actor* actor) {
    if(!actor || actor->IsPlayerRef() || actor->IsDead() || !actor->Is3DLoaded())return RE::BSContainer::ForEachResult::kContinue;
    auto center=vec(actor->GetPosition());center.z+=45;
    RE::NiPoint3 native_velocity{};actor->GetLinearVelocity(native_velocity);
    for(int which=0;which<2;++which) {
      if(which && !(rendered.flags&BallValid))continue;
      const auto& body=which?rendered.ball:rendered.car;
      const auto collision_offset=which?Vec3{}:transform.velocity(rotate(body.rotation,{13.87566f,0,20.75499f}));
      const auto position=transform.position(body.position)+collision_offset;
      if(length(center-position)>400)continue;
      if(now-npc_hits[actor->GetFormID()]<cfg.npc_cooldown)continue;
      auto from=position;
      if(continuous){const auto& previous=which?previous_impact.ball:previous_impact.car;auto old=transform.position(previous.position)+(which?Vec3{}:transform.velocity(rotate(previous.rotation,{13.87566f,0,20.75499f})));if(length(old-position)<400)from=old;}
      const auto extent=which?Vec3{93.15f,93.15f,93.15f}:Vec3{43.349705f,60.253689f,19.329536f};
      float fraction{};Vec3 contact_normal{};
      if(!sweep_actor(from,position,transform.rotation(body.rotation),extent*cfg.scale,center,22,40,&fraction,&contact_normal))continue;
      const auto contact_position=from+(position-from)*fraction;
      auto direction=center-contact_position;direction.z=std::clamp(direction.z,-15.0f,15.0f);
      const auto distance=length(direction);if(distance<.01f)continue;direction=direction*(1/distance);
      if(length(contact_normal)>.5f)direction=contact_normal;
      // Angular velocity is an axial vector: reflection reverses its sign.
      // Include the flipping body's contact velocity, rather than only its center speed.
      const auto rotation=transform.rotation(body.rotation);
      auto local=rotate({-rotation.x,-rotation.y,-rotation.z,rotation.w},center-contact_position);
      const auto bounds=extent*cfg.scale;
      local={std::clamp(local.x,-bounds.x,bounds.x),std::clamp(local.y,-bounds.y,bounds.y),std::clamp(local.z,-bounds.z,bounds.z)};
      const auto lever=rotate(rotation,local);
      const auto angular=transform.direction(body.angular_velocity)*(-1);
      const auto relative=transform.velocity(body.velocity)+cross(angular,lever)-vec(native_velocity);
      const auto closing=dot(relative,direction)/cfg.scale;
      const auto impact=actor_impact(closing,which?30.0f:180.0f,cfg.npc_damage_scale);
      if(impact.damage<=0)continue;
      npc_hits[actor->GetFormID()]=now;
      auto* player=RE::PlayerCharacter::GetSingleton();
      actor->HandleHealthDamage(player,impact.damage);actor->SetBeenAttacked(true);
      if(auto* process=actor->GetActorRuntimeData().currentProcess)process->KnockExplosion(actor,vec(center-direction*100),std::clamp(impact.launch_speed/100,2.0f,25.0f));
      auto launch=direction*(impact.launch_speed*cfg.scale);launch.z=std::max(launch.z,impact.launch_speed*cfg.scale*.12f);
      npc_launches.push_back({actor->GetHandle(),launch,now+50000,now+750000});
      send_event(which?EventType::ActorContactBall:EventType::ActorContactCar,transform.inverse_position(contact_position-collision_offset-direction*2),transform.inverse_direction(direction)*(-impact.body_recoil));
      spdlog::info("NPC {} impact {:08X}: closing {} damage {} launch {}",which?"ball":"car",actor->GetFormID(),closing,impact.damage,impact.launch_speed);
      break;
    }
    return RE::BSContainer::ForEachResult::kContinue;
  });
  previous_impact=rendered;have_previous_impact=true;
}
void interact() {
  auto* player=RE::PlayerCharacter::GetSingleton();auto* cell=player?player->GetParentCell():nullptr;
  if(!cell)return;
  const auto position=transform.position(rendered.car.position),forward=transform.direction(rotate(rendered.car.rotation,{1,0,0}));
  RE::TESObjectREFR* target=nullptr;float best=1e9f;
  cell->ForEachReference([&](RE::TESObjectREFR* ref) {
    if(!ref || ref==player || ref->IsDisabled() || !ref->Is3DLoaded())return RE::BSContainer::ForEachResult::kContinue;
    const auto* name=ref->GetDisplayFullName();if(!name || !*name)return RE::BSContainer::ForEachResult::kContinue;
    auto* base=ref->GetBaseObject();if(!base)return RE::BSContainer::ForEachResult::kContinue;
    const auto type=base->GetFormType();
    switch(type){
      case RE::FormType::NPC:case RE::FormType::Door:case RE::FormType::Container:case RE::FormType::Activator:case RE::FormType::Furniture:
      case RE::FormType::Weapon:case RE::FormType::Armor:case RE::FormType::Ammo:case RE::FormType::Book:case RE::FormType::Scroll:
      case RE::FormType::AlchemyItem:case RE::FormType::Ingredient:case RE::FormType::Misc:case RE::FormType::KeyMaster:case RE::FormType::SoulGem:case RE::FormType::Flora:break;
      default:return RE::BSContainer::ForEachResult::kContinue;
    }
    const auto delta=vec(ref->GetPosition())-position;const auto distance=length(delta);
    if(distance>240 || distance<1 || dot(delta,forward)/distance<-.2f)return RE::BSContainer::ForEachResult::kContinue;
    const float score=distance-(type==RE::FormType::NPC?60.0f:0.0f);
    if(score<best){best=score;target=ref;}return RE::BSContainer::ForEachResult::kContinue;
  });
  if(target){conversation_target=target->CreateRefHandle();spdlog::info("Car interaction {:08X} {}",target->GetFormID(),target->GetDisplayFullName());target->ActivateRef(player,0,nullptr,1,false);}
  else RE::SendHUDMessage::ShowHUDMessage("Car: move closer to someone or something, then press B / Circle or E");
}
void frame(RE::PlayerCharacter* player) {
  const auto batch=receiver.drain();
  const auto now=now_us();
  receive_drops+=batch.dropped;receive_gap=std::max(receive_gap,batch.max_gap_us);
  for (const auto& packet:batch.packets) {
    if(packet.is_state) timeline.push(packet.state,packet.receipt);
    else {
      const auto& probe=packet.event;const auto type=static_cast<unsigned>(probe.type);
      if(probe.type==EventType::CarMenuInput && timeline.latest() && probe.header.session==timeline.latest()->header.session && now>=packet.receipt && now-packet.receipt<150000 && probe.position.z>=0 && probe.position.z<=65535){
        if(menu_input_session!=probe.header.session){menu_input_session=probe.header.session;menu_input_time=0;car_menu::close();}
        if(probe.header.timestamp_us>menu_input_time){menu_input_time=probe.header.timestamp_us;
          car_menu::input(probe.position.x,probe.position.y,static_cast<std::uint16_t>(probe.position.z),!active || native_menu() || probe.velocity.x<0);
        }
      }
      else if(probe.type==EventType::Interact && !car_menu::busy() && timeline.latest() && probe.header.session==timeline.latest()->header.session)interaction_requested=true;
      else if(type>=7 && type<=10 && length(probe.velocity)<300) {wheel_probes[type-7]=probe;wheel_probe_times[type-7]=packet.receipt;}
    }
  }
  if (!active && cfg.auto_start && !auto_start_suppressed && now-last_auto_attempt>1000000 && timeline.sample(now,0,cfg.timeout) && (timeline.latest()->flags&OnGround)) {
    last_auto_attempt=now;start();
  }
  if(!active && resume_after_world && !menu() && timeline.sample(now,0,cfg.timeout)){resume_after_world=false;start();}
  car_menu::tick();
  if (!active) return;
  if (menu()) {interaction_requested=false;send_event(EventType::BridgeActive,{},{});send_event(EventType::PauseDriving,{},{1,0,0});camera_ready=false;return;}
  send_event(EventType::PauseDriving,{},{});
  if(interaction_requested){interaction_requested=false;interact();if(menu())return;}
  const auto p=timeline.sample(now,cfg.delay,cfg.timeout);
  if (!p || !(p->flags&CarValid) || player->IsDead()) {
    spdlog::warn("Bridge stopping: stream {} car valid {} player dead {}",bool(p),p?bool(p->flags&CarValid):false,player->IsDead());
    stop();RE::SendHUDMessage::ShowHUDMessage("Rocket bridge stopped: stream lost or player unavailable");return;
  }
  if (player->GetWorldspace()!=saved_world || (!saved_world && player->GetParentCell()!=saved_cell)) { stop(false);resume_after_world=true;return; }
  if(reanchor_after_loading){reanchor_after_loading=false;anchor();send_event(EventType::TeleportCar,p->car.position,{},p->car.rotation);}
  const auto car_pos=transform.position(p->car.position);
  if (cfg.max_range>0 && length(car_pos-transform.sky_origin)>cfg.max_range) {
    stop();RE::SendHUDMessage::ShowHUDMessage("Rocket bridge: calibration range exceeded; re-anchor in a new area");return;
  }
  const auto prediction_age=timeline.prediction_age_us(now,cfg.delay);
  if(prediction_age) {++predicted_frames;prediction_max_age=std::max(prediction_max_age,prediction_age);}
  rendered=*p;
  player->SetPosition(vec(car_pos),true); // cell streaming follows the physical car
  hide_player(player);
  place(car_node.get(),car_pos,transform.rotation(p->car.rotation),cfg.car_scale);
  car_node->SetAppCulled((p->flags&Demolished)!=0);
  if (p->flags&BallValid) {
    place(ball_node.get(),transform.position(p->ball.position),transform.rotation(p->ball.rotation),cfg.ball_scale);
    ball_node->SetAppCulled(false);
  } else ball_node->SetAppCulled(true);
  direct_render::publish(true,car_node->world,ball_node->world,(p->flags&Demolished)==0,(p->flags&BallValid)!=0,(p->flags&Boosting)!=0);
  if (direct_render::available()) {car_node->SetAppCulled(true);ball_node->SetAppCulled(true);}
  camera_ready=cfg.follow_camera && (p->flags&CameraValid);
  if (camera_ready) if (auto* cam=RE::PlayerCamera::GetSingleton()) cam->GetRuntimeData2().worldFOV=p->camera.fov;
  terrain_surfaces(player,*timeline.latest());
  wheel_surfaces(player);
  send_event(EventType::BridgeActive,{},{});
  if(cfg.native_terrain && !cfg.terrain_mesh_path.empty() &&
      (!last_terrain_export || (now-last_terrain_export>2000000 && length(car_pos-last_terrain_position)>512))) {
    if(terrain_export::write(player,transform,p->header.session,now,cfg.terrain_mesh_path))last_terrain_position=car_pos;
    last_terrain_export=now;
  }
  if(cfg.native_terrain && !cfg.terrain_mesh_path.empty() && cfg.npc_bumps)
    terrain_export::dynamic(player,transform,p->header.session,now,cfg.terrain_mesh_path,cfg.npc_damage_scale,static_cast<unsigned>(cfg.npc_cooldown));
  else npc_bumps(car_pos,now);
  if (now-last_diagnostic>1000000) {
    last_diagnostic=now;
    spdlog::info("Stream timing: max receive gap {} us, dropped queue packets {}, interpolation {} us, underrun frames {}, max underrun {} us",receive_gap,receive_drops,cfg.delay,predicted_frames,prediction_max_age);
    receive_gap=0;predicted_frames=0;prediction_max_age=0;
    spdlog::info("Bridge frame {}: car ({}, {}, {}) ball valid {} camera {}",p->header.sequence,
      car_pos.x,car_pos.y,car_pos.z,(p->flags&BallValid)!=0,camera_ready);
    if (auto* view=RE::Main::WorldRootCamera();view && spdlog::should_log(spdlog::level::debug)) {
      const auto wanted=transform.position(p->camera.position);
      spdlog::debug("View diagnostics: camera calls {} position ({}, {}, {}) wanted ({}, {}, {}) car hidden {} parent hidden {}",
        camera_update_calls,view->world.translate.x,view->world.translate.y,view->world.translate.z,
        wanted.x,wanted.y,wanted.z,car_node->GetAppCulled(),car_node->parent->GetAppCulled());
      float x{},y{},z{};
      const bool projected=view->WorldPtToScreenPt3(car_node->world.translate,x,y,z,1e-5f);
      spdlog::debug("Car node: world ({}, {}, {}) bound {} screen ({}, {}, {}) projected {}",
        car_node->world.translate.x,car_node->world.translate.y,car_node->world.translate.z,
        car_node->worldBound.radius,x,y,z,projected);
      for (auto* root:{car_node.get(),ball_node.get()}) RE::BSVisit::TraverseScenegraphGeometries(root,[&](RE::BSGeometry* g) {
        spdlog::debug("Geometry {} culled {} bound {} position ({}, {}, {})",g->name.c_str(),g->GetAppCulled(),g->worldBound.radius,g->world.translate.x,g->world.translate.y,g->world.translate.z);
        const auto& data=g->GetGeometryRuntimeData();
        auto* shader=data.shaderProperty.get();
        spdlog::debug("Render data {} renderer {} descriptor {} bound center ({}, {}, {}) shader {} alpha {} flags {}",g->name.c_str(),static_cast<bool>(data.rendererData),static_cast<unsigned>(data.vertexDesc.GetFlags()),g->worldBound.center.x,g->worldBound.center.y,g->worldBound.center.z,static_cast<bool>(shader),shader?shader->QMaterialAlpha():-1,shader?shader->flags.underlying():0);
        return RE::BSVisit::BSVisitControl::kContinue;
      });
    }
  }
}
struct PlayerUpdate {
  static void thunk(RE::PlayerCharacter* player,float dt) {
    original(player,dt);
    try { frame(player); } catch (const std::exception& e) { spdlog::error("Frame failed: {}",e.what());stop(); }
  }
  static inline REL::Relocation<decltype(thunk)> original;
};
// Skyrim calls PlayerCamera::Update directly. Pin its final scene transform
// after smoothing/collision, using the actual NiCamera basis (as SkyCraft does).
struct CameraUpdate {
  static void thunk(RE::PlayerCamera* camera) {
    ++camera_update_calls;
    original(camera);
    const bool dialogue=RE::UI::GetSingleton()->IsMenuOpen("Dialogue Menu");
    const auto target=conversation_target.get();
    if (!active || (!dialogue && (!camera_ready || menu())) || !camera->cameraRoot) return;
    hide_player(RE::PlayerCharacter::GetSingleton());
    auto* view=RE::Main::WorldRootCamera();if (!view) return;
    auto* root=camera->cameraRoot.get();
    const auto relative=root->world.Invert()*view->world;
    RE::NiTransform desired;
    desired.translate=vec(transform.position(rendered.camera.position));desired.scale=1;
    auto rotation=camera_matrix(transform,rendered.camera.rotation);
    if(dialogue && target){
      auto eye=transform.position(rendered.car.position);eye.z+=95;
      auto aim=vec(target->GetPosition());aim.z+=110;
      auto forward=aim-eye;const float magnitude=length(forward);if(magnitude>.01f)forward=forward*(1/magnitude);
      auto right=cross(forward,{0,0,1});const float width=length(right);if(width>.01f)right=right*(1/width);else right={1,0,0};
      const auto up=cross(right,forward);desired.translate=vec(eye);
      rotation={{{forward.x,up.x,right.x},{forward.y,up.y,right.y},{forward.z,up.z,right.z}}};
    }
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) desired.rotate.entry[i][j]=rotation[i][j];
    const auto world=desired*relative.Invert();
    root->local=root->parent?root->parent->world.Invert()*world:world;
    RE::NiUpdateData update{};update.flags=RE::NiUpdateData::Flag::kDirty;root->Update(update);
    camera->GetRuntimeData2().pos=desired.translate;
    if (auto* sky=RE::Sky::GetSingleton();sky && sky->root) {
      sky->root->local.translate=desired.translate;sky->root->world.translate=desired.translate;
    }
  }
  static inline REL::Relocation<decltype(thunk)> original;
};
struct Controls {
  static RE::BSEventNotifyControl thunk(RE::PlayerControls* self,RE::InputEvent* const* event,RE::BSTEventSource<RE::InputEvent*>* source) {
    if (active && (car_menu::busy() || !native_menu())) return RE::BSEventNotifyControl::kContinue;
    return original(self,event,source);
  }
  static inline REL::Relocation<decltype(thunk)> original;
};
// Menu controls get gamepad events only while a Skyrim UI is open.
struct MenuInput {
  static RE::BSEventNotifyControl thunk(RE::MenuControls* self,RE::InputEvent* const* events,RE::BSTEventSource<RE::InputEvent*>* source){
    if(active && car_menu::busy())return RE::BSEventNotifyControl::kContinue;
    if(!active || native_menu() || !events)return original(self,events,source);
    std::vector<std::pair<RE::InputEvent*,RE::InputEvent*>> links;RE::InputEvent* first=nullptr;RE::InputEvent* tail=nullptr;
    for(auto* e=*events;e;e=e->next){links.emplace_back(e,e->next);if(e->device.get()==RE::INPUT_DEVICE::kGamepad)continue;if(tail)tail->next=e;else first=e;tail=e;}
    if(tail)tail->next=nullptr;
    auto result=original(self,&first,source);
    for(const auto& link:links)link.first->next=link.second;
    return result;
  }
  static inline REL::Relocation<decltype(thunk)> original;
};
struct MenuEvents : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
  RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override{
    if(!e)return RE::BSEventNotifyControl::kContinue;
    const std::string name=e->menuName.c_str();
    auto* ui=RE::UI::GetSingleton();auto native=ui?ui->GetMenu(name):nullptr;
    const bool interactive=native && (native->UsesMenuContext() || native->PausesGame() || native->Modal());
    if(interactive || blocking_menus.contains(name) || name=="Dialogue Menu" || name=="InventoryMenu" || name=="ContainerMenu" || name=="BarterMenu" || name=="Console" || name=="Loading Menu" || name=="Main Menu" || name=="TweenMenu" || name=="MapMenu" || name=="StatsMenu" || name=="Journal Menu" || name=="MessageBoxMenu" || name=="MagicMenu" || name=="FavoritesMenu" || name=="Sleep/Wait Menu"){
      if(e->opening){car_menu::native_opened();blocking_menus.insert(name);if(name=="Loading Menu" && active)reanchor_after_loading=true;}else blocking_menus.erase(name);
      if(active){send_event(EventType::BridgeActive,{},{});send_event(EventType::PauseDriving,{}, {blocking_menus.empty()?0.0f:1.0f,0,0});}
      if(name=="Dialogue Menu" && !e->opening)conversation_target={};
    }
    return RE::BSEventNotifyControl::kContinue;
  }
} menu_events;
struct Input : RE::BSTEventSink<RE::InputEvent*> {
  RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events,RE::BSTEventSource<RE::InputEvent*>*) override {
    if(!events)return RE::BSEventNotifyControl::kContinue;
    if(car_menu::busy()){
      for(auto* event=*events;event;event=event->next)if(auto* button=event->AsButtonEvent();button && button->device.get()==RE::INPUT_DEVICE::kKeyboard && button->IsDown() && button->GetIDCode()==1)car_menu::close();
      return RE::BSEventNotifyControl::kContinue;
    }
    if(menu())return RE::BSEventNotifyControl::kContinue;
    for (auto* event=*events;event;event=event->next) {
      auto* button=event->AsButtonEvent();
      if (!button || button->device.get()!=RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) continue;
      const auto key=button->GetIDCode();
      if (key==static_cast<std::uint32_t>(cfg.toggle_key)) {
        if (active) { auto_start_suppressed=true;stop(); }
        else { auto_start_suppressed=false;start(); }
      }
      else if(key==18 && active)interact();
      else if (key==static_cast<std::uint32_t>(cfg.anchor_key) && active) anchor();
      else if (key==static_cast<std::uint32_t>(cfg.ball_key) && active) {
        const auto* p=timeline.latest();
        if (p) send_event(EventType::SetBall,p->car.position+rotate(p->car.rotation,{300,0,100}),{});
      }
    }
    return RE::BSEventNotifyControl::kContinue;
  }
} input;
void install() {
  socket.open(static_cast<std::uint16_t>(cfg.state_port));
  receiver.start();
  REL::Relocation<std::uintptr_t> player{RE::VTABLE_PlayerCharacter[0]};
  PlayerUpdate::original=player.write_vfunc(0xAD,PlayerUpdate::thunk);
  const auto camera_target=REL::Relocation<std::uintptr_t>{RELOCATION_ID(49852,50784)}.address();
  const auto text=REL::Module::get().segment(REL::Segment::textx);
  const auto* code=reinterpret_cast<const std::uint8_t*>(text.address());
  std::vector<std::uintptr_t> camera_calls;
  for (std::size_t i=0;i+5<=text.size();++i) {
    if (code[i]!=0xE8) continue;
    std::int32_t offset;std::memcpy(&offset,code+i+1,4);
    if (text.address()+i+5+static_cast<std::intptr_t>(offset)==camera_target) camera_calls.push_back(text.address()+i);
  }
  if (camera_calls.empty()) throw std::runtime_error("PlayerCamera update call site not found");
  SKSE::AllocTrampoline(camera_calls.size()*16+512);
  for (const auto site:camera_calls) CameraUpdate::original=SKSE::GetTrampoline().write_call<5>(site,CameraUpdate::thunk);
  spdlog::info("PlayerCamera::Update: hooked {} call sites",camera_calls.size());
  direct_render::install();
  REL::Relocation<std::uintptr_t> controls{RE::VTABLE_PlayerControls[0]};
  Controls::original=controls.write_vfunc(0x1,Controls::thunk);
  REL::Relocation<std::uintptr_t> menu_controls{RE::VTABLE_MenuControls[0]};
  MenuInput::original=menu_controls.write_vfunc(0x1,MenuInput::thunk);
  RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&menu_events);
  RE::BSInputDeviceManager::GetSingleton()->AddEventSink(&input);
  spdlog::info("Listening on 127.0.0.1:{}; F8 toggles",cfg.state_port);
}
}
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
  SKSE::Init(skse);
  const auto logs=SKSE::log::log_directory().value_or(std::filesystem::path("Data/SKSE/Plugins"));
  std::filesystem::create_directories(logs);
  auto sink=std::make_shared<spdlog::sinks::basic_file_sink_mt>((logs/"SkyrimRocketBridge.log").string(),true);
  spdlog::set_default_logger(std::make_shared<spdlog::logger>("bridge",sink));spdlog::flush_on(spdlog::level::info);
  if (REL::Module::IsVR()) { spdlog::error("Skyrim VR is not supported by this plugin");return false; }
  cfg.load();
  return SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) {
    if (message->type==SKSE::MessagingInterface::kDataLoaded) {
      try {
        install();
        suppress_survival_prompt();
        // The regular task queue does not advance while the main menu is
        // paused. The UI queue does; CenterOnCell requests the engine's own
        // asynchronous cell transition without fabricating input events.
        if (cfg.test_cell) SKSE::GetTaskInterface()->AddUITask([] {
          auto* player=RE::PlayerCharacter::GetSingleton();
          const bool accepted=player && player->CenterOnCell("qasmoke");
          spdlog::info("Requested temporary qasmoke test cell: {}",accepted);
        });
      } catch (const std::exception& e) { spdlog::error("Plugin inactive: {}",e.what()); }
    } else if (message->type==SKSE::MessagingInterface::kPreLoadGame || message->type==SKSE::MessagingInterface::kNewGame) {
      stop(false);timeline.clear();
    } else if (message->type==SKSE::MessagingInterface::kPostLoadGame) {
      suppress_survival_prompt();
    } else if (message->type==SKSE::MessagingInterface::kSaveGame) {
      stop(); // temporary scene visuals and driving state must not be saved
    }
  });
}
