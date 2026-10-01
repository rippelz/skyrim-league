#include "NativeTerrain.hpp"
#include "DynamicContacts.hpp"
#include <BulletDynamics/Dynamics/btRigidBody.h>
#include "shared/terrain_mesh.hpp"
#include "shared/udp.hpp"
#include <Windows.h>
#include <MinHook.h>
#include <BulletCollision/CollisionDispatch/btCollisionObject.h>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleIndexVertexArray.h>
#include <atomic>
#include <mutex>
#include <fstream>
#include <sstream>
#include <array>
#include <cfloat>

namespace native_terrain {
namespace {
bool readable(const void* p,std::size_t n) {
  if(!p || !n)return false;MEMORY_BASIC_INFORMATION m{};
  if(!VirtualQuery(p,&m,sizeof m) || m.State!=MEM_COMMIT || (m.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
  return reinterpret_cast<std::uintptr_t>(p)+n<=reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize;
}
template<class T>void write_field(void* p,std::size_t offset,const T& value) {std::memcpy(static_cast<char*>(p)+offset,&value,sizeof value);}
template<class T>T field(const void* p,std::size_t offset) {T v{};std::memcpy(&v,static_cast<const char*>(p)+offset,sizeof v);return v;}
struct NativeArray {int allocator,size,capacity,pad;void** data;bool owns;};
static_assert(sizeof(NativeArray)==32);
struct Mesh {
  bridge::TerrainHeader header;
  std::vector<float> vertices;
  std::vector<std::uint32_t> indices;
};
void* unique_pattern() {
  auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));if(!base)return nullptr;
  auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
  // RLArenaCollisionDumper's world-step signature. Matches internalSingleStepSimulation;
  // the installed build was disassembled to verify its (world, float dt) ABI.
  const int pattern[]={0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x8b,0x01,0x48,0x8b,0xd9,0x0f,0x29,0x74,0x24,-1,0x0f,0x28,0xf1,0xff,0x90,-1,-1,-1,-1,0x48,0x8b,0x03,0x0f,0x28,0xce,0x48,0x8b,0xcb,0xff,0x90};
  void* found{};int count=0;auto* section=IMAGE_FIRST_SECTION(nt);
  for(unsigned s=0;s<nt->FileHeader.NumberOfSections;++s) {
    if(!(section[s].Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
    auto* start=base+section[s].VirtualAddress;auto size=section[s].Misc.VirtualSize;
    for(std::size_t i=0;i+sizeof(pattern)/sizeof(*pattern)<=size;++i) {
      bool match=true;for(unsigned j=0;j<sizeof(pattern)/sizeof(*pattern);++j)if(pattern[j]>=0 && start[i+j]!=pattern[j]){match=false;break;}
      if(match){found=start+i;++count;}
    }
  }
  return count==1?found:nullptr;
}
}
struct Backend::Impl {
  using Step=void(__fastcall*)(void*,float);
  static inline Impl* instance{};
  Step original{};void* target{};
  std::function<void(std::string)> log;
  std::mutex mutex;
  std::shared_ptr<Mesh> pending,current;
  std::atomic<bool> active{false},installed{false};
  bridge::Vec3 car{},ball{};
  dynamic_contacts::Backend dynamics;
  std::uint64_t session{},last_check{},last_generation{},last_dynamic_log{};
  void* selected_world{};
  std::vector<std::shared_ptr<Mesh>> retired;
  struct Saved {btCollisionObject* object;btCollisionShape* shape;btTransform transform;int flags;void* proxy;short group,mask;};
  std::vector<Saved> arena;
  btBvhTriangleMeshShape* primary_shape{};
  btTriangleIndexVertexArray* primary_interface{};
  btIndexedMesh original_mesh{};
  btVector3 original_scaling{1,1,1},original_min,original_max;
  using BuildBvh=void(__fastcall*)(void*);
  BuildBvh build_bvh{};
  std::vector<std::string> messages;
  std::filesystem::path trace_path;
  void trace(const char* phase) {
    if(trace_path.empty() || !primary_interface)return;
    auto m=primary_interface->getIndexedMeshArray()[0];
    std::ofstream out(trace_path,std::ios::app);
    out<<bridge::now_us()<<" "<<phase<<" shape="<<primary_shape<<" interface="<<primary_interface
       <<" count="<<m.m_numTriangles<<" vertices="<<static_cast<const void*>(m.m_vertexBase)
       <<" indices="<<static_cast<const void*>(m.m_triangleIndexBase)<<" current="<<current.get()<<" pending="<<pending.get()<<"\n";
  }
  bool world_valid(void* world) {
    if(!readable(world,48))return false;
    auto a=field<NativeArray>(world,8);
    if(a.size<3 || a.size>20000 || a.capacity<a.size || !readable(a.data,a.size*sizeof(void*)))return false;
    bool nearby=false,mesh=false;
    for(int i=0;i<a.size;++i) {
      auto* object=static_cast<btCollisionObject*>(a.data[i]);if(!readable(object,0x130))return false;
      auto* shape=field<btCollisionShape*>(object,0xd0);if(!readable(shape,0x20))return false;
      auto type=field<int>(shape,8);if(type<0||type>35)return false;
      auto flags=field<int>(object,0xe8);
      mesh|=(type==21 && (flags&1));
      if(!(flags&3)) {
        auto pos=field<btVector3>(object,0x40); // worldTransform.origin: vptr + 16-byte alignment + 48-byte basis
        const bridge::Vec3 rl{pos.x()*50,pos.y()*50,pos.z()*50};
        nearby|=bridge::length(rl-car)<30;
      }
    }
    return mesh&&nearby;
  }
  bool snapshot(void* world) {
    auto a=field<NativeArray>(world,8);
    std::vector<Saved> saved;
    for(int i=0;i<a.size;++i) {
      auto* object=static_cast<btCollisionObject*>(a.data[i]);
      auto* shape=field<btCollisionShape*>(object,0xd0);auto type=field<int>(shape,8);auto flags=field<int>(object,0xe8);
      if(!(flags&1) || (type!=21 && type!=28 && type!=0 && type!=31 && type!=4))continue;
      // Verify the stock MSVC Bullet shape ABI before invoking any virtual method.
      auto* name=shape->getName();if(!readable(name,16))continue;
      if(type==21 && std::memcmp(name,"BVHTRIANGLEMESH",15)==0 && !primary_shape) {
        auto* iface=field<btTriangleIndexVertexArray*>(shape,0x40);
        if(!readable(iface,0x40))continue;
        auto parts=field<NativeArray>(iface,0x20);
        if(parts.size!=1 || !readable(parts.data,sizeof(btIndexedMesh)))continue;
        auto first=field<btIndexedMesh>(parts.data,0);
        if(first.m_numTriangles<1 || first.m_numVertices<3 || first.m_vertexStride<12 || first.m_vertexStride>64)continue;
        original_scaling=shape->getLocalScaling();original_min=field<btVector3>(shape,0x20);original_max=field<btVector3>(shape,0x30);
        auto* vt=field<void*>(shape,0);auto* setter=static_cast<unsigned char*>(field<void*>(vt,0x30));
        // Verified native BVH setter ends by calling RL's own allocator/rebuilder.
        const unsigned char prefix[]={0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x30,0x48,0x8b,0xd9,0x80,0x79,0x59,0x00};
        if(!readable(setter,0x62) || setter[0x5d]!=0xe8)return false;
        auto* builder=setter+0x62+field<std::int32_t>(setter,0x5e);
        if(!readable(builder,sizeof prefix) || std::memcmp(builder,prefix,sizeof prefix)!=0)return false;
        build_bvh=reinterpret_cast<BuildBvh>(builder);
        primary_shape=static_cast<btBvhTriangleMeshShape*>(shape);primary_interface=iface;original_mesh=first;
      }
      auto* proxy=field<void*>(object,0xc8);
      const bool proxy_valid=readable(proxy,32) && field<void*>(proxy,0)==object;
      short group=proxy_valid?field<short>(proxy,8):0,mask=proxy_valid?field<short>(proxy,10):0;
      saved.push_back({object,shape,object->getWorldTransform(),flags,proxy_valid?proxy:nullptr,group,mask});
      messages.push_back("Native static shape="+std::string(name)+" flags="+std::to_string(flags)+" filter="+std::to_string(group)+","+std::to_string(mask));
    }
    if(!primary_shape || saved.empty())return false;
    arena=std::move(saved);selected_world=world;
    messages.push_back("Native terrain validated Bullet arena: "+std::to_string(arena.size())+" static bodies, "+std::to_string(original_mesh.m_numTriangles)+" source triangles");
    return true;
  }
  void clean_contacts() {
    // Verified against RL's removeCollisionObject implementation: dispatcher +0x28,
    // broadphase +0x58, getOverlappingPairCache vslot +0x48, cleanProxyFromPairs +0x48.
    // Destroy cached algorithms before changing triangle indices or collision filters.
    auto* broadphase=field<void*>(selected_world,0x58);
    auto* dispatcher=field<void*>(selected_world,0x28);
    auto* bv=field<void*>(broadphase,0);
    using GetCache=void*(__fastcall*)(void*);
    auto* cache=reinterpret_cast<GetCache>(field<void*>(bv,0x48))(broadphase);
    auto* cv=field<void*>(cache,0);
    using Clean=void(__fastcall*)(void*,void*,void*);
    auto clean=reinterpret_cast<Clean>(field<void*>(cv,0x48));
    for(auto& s:arena)if(s.proxy)clean(cache,s.proxy,dispatcher);
  }
  void rebuild(const btVector3& scaling,const btVector3& lo,const btVector3& hi) {
    // Do not use setLocalScaling: its parent recomputes support through the OLD
    // BVH, whose triangle IDs may exceed the replacement mesh's size.
    auto& native_scaling=const_cast<btVector3&>(primary_shape->getLocalScaling());
    native_scaling=scaling;write_field(primary_shape,0x20,lo);write_field(primary_shape,0x30,hi);
    build_bvh(primary_shape);
  }
  void restore() {
    if(!installed)return;
    trace("restore-before");clean_contacts();
    if(readable(primary_interface,0x40)) {
      primary_interface->getIndexedMeshArray()[0]=original_mesh;rebuild(original_scaling,original_min,original_max);trace("restore-after");
    }
    for(auto& s:arena)if(readable(s.object,0x130)) {write_field(s.object,0xd0,s.shape);write_field(s.object,0x10,s.transform);write_field(s.object,0xe8,s.flags);if(s.proxy && readable(s.proxy,32)){write_field(s.proxy,8,s.group);write_field(s.proxy,10,s.mask);}write_field(s.object,0xf4,int{1});}
    installed=false;if(current)retired.push_back(current);current.reset();arena.clear();primary_shape=nullptr;primary_interface=nullptr;selected_world=nullptr;
    messages.push_back("Native terrain restored original arena collision");
  }
  void observe(void* world) {
    std::scoped_lock lock(mutex);
    // Mesh-backed contact algorithms were destroyed before replacement. Retain the
    // previous buffers through the rest of that physics step, then release them.
    // Retain three generations for native query lifetime across physics steps.
    if(retired.size()>3)retired.erase(retired.begin(),retired.end()-3);
    if(!active) {if(installed && world==selected_world)restore();return;}
    if(!pending)return;
    if(!installed && (!world_valid(world)||!snapshot(world)))return;
    if(world!=selected_world)return;
    if(installed && pending->header.generation==current->header.generation)return;
    auto next=pending;
    clean_contacts();
    btIndexedMesh indexed{};indexed.m_numTriangles=next->header.triangles;indexed.m_numVertices=next->header.triangles*3;
    indexed.m_triangleIndexBase=reinterpret_cast<const unsigned char*>(next->indices.data());indexed.m_triangleIndexStride=12;
    indexed.m_vertexBase=reinterpret_cast<const unsigned char*>(next->vertices.data());indexed.m_vertexStride=12;indexed.m_indexType=PHY_INTEGER;indexed.m_vertexType=PHY_FLOAT;
    btVector3 lo(FLT_MAX,FLT_MAX,FLT_MAX),hi(-FLT_MAX,-FLT_MAX,-FLT_MAX);
    for(std::size_t i=0;i<next->vertices.size();i+=3) {btVector3 v(next->vertices[i],next->vertices[i+1],next->vertices[i+2]);lo.setMin(v);hi.setMax(v);}
    // Conservative bounds include a small native-unit margin; exact triangles
    // still determine contacts and suspension hits.
    lo-=btVector3(.04f,.04f,.04f);hi+=btVector3(.04f,.04f,.04f);
    trace("replace-before");primary_interface->getIndexedMeshArray()[0]=indexed;rebuild(btVector3(1,1,1),lo,hi);trace("replace-after");
    for(auto& s:arena) {
      if(s.shape==primary_shape) {write_field(s.object,0xd0,primary_shape);write_field(s.object,0x10,btTransform::getIdentity());write_field(s.object,0xe8,int{1});if(s.proxy){write_field(s.proxy,8,s.group);write_field(s.proxy,10,s.mask);}}
      else {
        // Disable stock geometry for both rigid-body contacts and suspension ray tests.
        // Keep the native shapes and transforms intact so arena ownership is unchanged.
        write_field(s.object,0xe8,int{s.flags|4});
        if(s.proxy){write_field(s.proxy,8,short{0});write_field(s.proxy,10,short{0});}
      }
      write_field(s.object,0xf4,int{1});
    }

    messages.push_back("Native terrain BVH bounds z="+std::to_string(lo.z())+","+std::to_string(hi.z()));
    if(current)retired.push_back(current);current=next;installed=true;
    messages.push_back("Native terrain installed "+std::to_string(indexed.m_numTriangles)+" Skyrim triangles in RL physics; generation "+std::to_string(next->header.generation));
  }
  void contacts(void* world,float dt) {
    std::scoped_lock lock(mutex);
    if(!installed || !active){dynamics.clear();return;}
    if(world!=selected_world)return;
    auto a=field<NativeArray>(world,8);btRigidBody* drivers[2]{};
    for(int i=0;i<a.size;++i){
      auto* o=static_cast<btCollisionObject*>(a.data[i]);
      if(!readable(o,sizeof(btRigidBody)) || o->getInternalType()!=btCollisionObject::CO_RIGID_BODY || o->isStaticOrKinematicObject())continue;
      auto* body=static_cast<btRigidBody*>(o);const auto pos=body->getWorldTransform().getOrigin();
      const bridge::Vec3 wire{pos.x()*50,pos.y()*50,pos.z()*50};
      // Guard the additional rigid-body ABI against known native RL masses before
      // reading/writing velocities. Do not attach to unrelated physics bodies.
      const auto inv=body->getInvMass();
      if(std::abs(inv-1/180.f)<.00001f && bridge::length(wire-car)<30)drivers[0]=body;
      if(std::abs(inv-1/30.f)<.00001f && bridge::length(wire-ball)<30)drivers[1]=body;
    }
    if(drivers[0])dynamics.step(drivers[0],drivers[1],dt,primary_shape,current?current->header.generation:0);
    const auto now=bridge::now_us();if(now-last_dynamic_log>1000000){last_dynamic_log=now;messages.push_back("Dynamic collision car_abi="+std::to_string(drivers[0]!=nullptr)+" ball_abi="+std::to_string(drivers[1]!=nullptr)+" "+dynamics.diagnostics());}
  }
  static void __fastcall step(void* world,float dt) {
    auto* self=instance;
    if(self){self->observe(world);self->contacts(world,dt);}
    if(self && self->original)self->original(world,dt);
  }
};
Backend::Backend():impl_(std::make_unique<Impl>()){}
Backend::~Backend(){shutdown();}
bool Backend::initialize(std::function<void(std::string)> log) {
  if(impl_->original)return true;
  impl_->log=std::move(log);impl_->target=unique_pattern();
  if(!impl_->target){impl_->log("Native terrain unavailable: physics-step signature is missing or ambiguous");return false;}
  const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
  Impl::instance=impl_.get();
  if(MH_CreateHook(impl_->target,reinterpret_cast<void*>(&Impl::step),reinterpret_cast<void**>(&impl_->original))!=MH_OK || MH_EnableHook(impl_->target)!=MH_OK) {Impl::instance=nullptr;return false;}
  impl_->log("Native terrain physics-step hook ready (offline only)");return true;
}
void Backend::update(const std::filesystem::path& path,std::uint64_t session,bridge::Vec3 car,bool active,bridge::Vec3 ball,bool dynamic) {
  if(!impl_->original)return;
  {
    std::scoped_lock lock(impl_->mutex);impl_->car=car;impl_->ball=ball;impl_->active=active;
    if(!active){impl_->pending.reset();impl_->last_generation=0;}
    for(auto& msg:impl_->messages)impl_->log(msg);impl_->messages.clear();
    if(impl_->session!=session) {impl_->session=session;impl_->pending.reset();impl_->last_generation=0;}
  }
  impl_->dynamics.update(path,session,active && dynamic);
  const auto now=bridge::now_us();if(!active || now-impl_->last_check<250000)return;impl_->last_check=now;
  std::ifstream file(path,std::ios::binary);if(!file)return;
  bridge::TerrainHeader h{};file.read(reinterpret_cast<char*>(&h),sizeof h);
  if(!file || !bridge::valid(h) || h.session!=session || h.generation<=impl_->last_generation)return;
  auto mesh=std::make_shared<Mesh>();mesh->header=h;mesh->vertices.reserve(h.triangles*9);mesh->indices.reserve(h.triangles*3);
  for(unsigned i=0;i<h.triangles;++i) {
    bridge::TerrainTriangle tri{};file.read(reinterpret_cast<char*>(&tri),sizeof tri);if(!file||!bridge::valid(tri))return;
    for(auto p:{tri.a,tri.b,tri.c}) {mesh->indices.push_back(mesh->indices.size());mesh->vertices.insert(mesh->vertices.end(),{p.x/50,p.y/50,(p.z+bridge::kNativeTerrainOffset.z)/50});}
  }
  if(file.peek()!=std::char_traits<char>::eof())return;
  std::scoped_lock lock(impl_->mutex);impl_->last_generation=h.generation;impl_->pending=std::move(mesh);
}
bool Backend::applied()const{return impl_->installed;}
void Backend::shutdown() {
  if(!impl_||!impl_->original)return;
  impl_->active=false;
  // The hook restores on the next physics tick; a paused world is already stopped.
  for(int i=0;i<20 && impl_->installed;++i)Sleep(10);
  MH_DisableHook(impl_->target);
  {std::scoped_lock lock(impl_->mutex);if(impl_->installed)impl_->restore();}
  impl_->dynamics.clear();
  MH_RemoveHook(impl_->target);Impl::instance=nullptr;impl_->original=nullptr;
}
}
