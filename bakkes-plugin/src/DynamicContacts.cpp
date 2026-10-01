#include "DynamicContacts.hpp"
#include "shared/terrain_mesh.hpp"
#include "shared/udp.hpp"
#include <btBulletDynamicsCommon.h>
#ifdef _WIN32
#include <Windows.h>
#endif
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
namespace dynamic_contacts {
namespace {
btVector3 v(bridge::Vec3 p){return {p.x,p.y,p.z};}
bridge::Vec3 v(const btVector3& p){return {p.x(),p.y(),p.z()};}
btTransform pose(const bridge::Body& b){return btTransform(btQuaternion(b.rotation.x,b.rotation.y,b.rotation.z,b.rotation.w),v((b.position+bridge::kNativeTerrainOffset)*(1/50.f)));}
struct DriverShape {
  std::vector<std::unique_ptr<btCollisionShape>> storage;
  btCollisionShape* clone(const btCollisionShape* source,int depth=0){
    if(!source || depth>8)return nullptr;std::unique_ptr<btCollisionShape> shape;
    if(source->getShapeType()==BOX_SHAPE_PROXYTYPE){
      const auto* box=static_cast<const btBoxShape*>(source);const auto extent=box->getHalfExtentsWithMargin();
      if(!bridge::valid(v(extent)) || extent.x()<=0 || extent.y()<=0 || extent.z()<=0 || extent.length()>100)return nullptr;
      shape=std::make_unique<btBoxShape>(extent);shape->setMargin(source->getMargin());
    }else if(source->getShapeType()==SPHERE_SHAPE_PROXYTYPE){
      const auto radius=static_cast<const btSphereShape*>(source)->getRadius();if(!std::isfinite(radius)||radius<=0||radius>100)return nullptr;
      shape=std::make_unique<btSphereShape>(radius);
    }else if(source->getShapeType()==COMPOUND_SHAPE_PROXYTYPE){
      const auto* compound=static_cast<const btCompoundShape*>(source);const auto count=compound->getNumChildShapes();if(count<1||count>32)return nullptr;
      auto copy=std::make_unique<btCompoundShape>(false);
      for(int i=0;i<count;++i){auto* child=clone(compound->getChildShape(i),depth+1);if(!child)return nullptr;copy->addChildShape(compound->getChildTransform(i),child);}
      shape=std::move(copy);
    }else return nullptr;
    auto* result=shape.get();storage.push_back(std::move(shape));return result;
  }
};
struct Snapshot {bridge::DynamicHeader header;std::vector<bridge::DynamicBody> bodies;std::vector<bridge::Vec3> vertices;};
struct Proxy {
  std::unique_ptr<btConvexHullShape> shape;
  std::unique_ptr<btRigidBody> body;
  bridge::DynamicBody record;
  std::uint64_t generation{};
};
}
struct Backend::Impl {
  std::mutex mutex;
  std::shared_ptr<Snapshot> pending;
  std::uint64_t session{},last_read{},last_flush{},sequence{},waiting{};
  bool active{};
  std::filesystem::path responses;
  btDefaultCollisionConfiguration config;
  btCollisionDispatcher dispatcher{&config};
  btDbvtBroadphase broadphase;
  btSequentialImpulseConstraintSolver solver;
  btDiscreteDynamicsWorld world{&dispatcher,&broadphase,&solver,&config};
  std::unordered_map<std::uint64_t,Proxy> proxies;
  std::unique_ptr<btRigidBody> drivers[2];
  btCollisionShape* driver_shapes[2]{};btRigidBody* driver_sources[2]{};
  std::unique_ptr<DriverShape> owned_shapes[2];
  std::vector<bridge::DynamicContact> outgoing;
  std::uint64_t loaded{};
  std::unique_ptr<btRigidBody> ground;btCollisionShape* ground_shape{};std::uint64_t ground_generation{};
  Impl(){world.setGravity({0,0,0});world.getSolverInfo().m_numIterations=20;world.getSolverInfo().m_splitImpulse=true;}
  void clear(){
    for(auto& [id,p]:proxies)world.removeRigidBody(p.body.get());proxies.clear();
    for(int i=0;i<2;++i){if(drivers[i])world.removeRigidBody(drivers[i].get());drivers[i].reset();owned_shapes[i].reset();driver_shapes[i]=nullptr;driver_sources[i]=nullptr;}
    if(ground)world.removeRigidBody(ground.get());ground.reset();ground_shape=nullptr;ground_generation=0;
    outgoing.clear();loaded=0;last_flush=0;waiting=0;
  }
  ~Impl(){clear();}
  void synchronize(const Snapshot& snapshot) {
    std::unordered_set<std::uint64_t> seen;
    for(const auto& record:snapshot.bodies) {
      seen.insert(record.id);auto& p=proxies[record.id];
      if(!p.body || p.record.shape!=record.shape || p.record.mass!=record.mass || p.record.flags!=record.flags) {
        if(p.body)world.removeRigidBody(p.body.get());p.body.reset();
        p.shape=std::make_unique<btConvexHullShape>();
        for(unsigned j=0;j<record.count;++j)p.shape->addPoint(v(snapshot.vertices[record.first+j]*(1/50.f)),false);
        p.shape->recalcLocalAabb();p.shape->setMargin(.01f);
        btVector3 inertia{0,0,0};if(record.mass>0)p.shape->calculateLocalInertia(record.mass,inertia);
        btRigidBody::btRigidBodyConstructionInfo info(record.mass,nullptr,p.shape.get(),inertia);
        p.body=std::make_unique<btRigidBody>(info);p.body->setActivationState(DISABLE_DEACTIVATION);
        if(record.mass==0)p.body->setCollisionFlags(btCollisionObject::CF_KINEMATIC_OBJECT);
        p.body->setUserIndex(-1);p.body->setUserPointer(&p);world.addRigidBody(p.body.get(),2,1|4);
      }
      p.record=record;p.generation=snapshot.header.generation;
      // Preserve the locally solved displacement between snapshots. Havok is the
      // authority once the next snapshot (with our point impulses applied) arrives.
      p.body->setCenterOfMassTransform(pose(record.state));
      p.body->setInterpolationWorldTransform(p.body->getWorldTransform());
      p.body->setLinearVelocity(v(record.state.velocity*(1/50.f)));
      p.body->setAngularVelocity(v(record.state.angular_velocity));
      p.body->setFriction(record.friction);p.body->setRestitution(record.restitution);
      world.updateSingleAabb(p.body.get());
    }
    for(auto it=proxies.begin();it!=proxies.end();)if(!seen.contains(it->first)){world.removeRigidBody(it->second.body.get());it=proxies.erase(it);}else ++it;
    loaded=snapshot.header.generation;
  }
  void flush(std::uint64_t now) {
    if(outgoing.empty() || (waiting && pending && pending->header.ack<waiting))return;
    if(now-last_flush<16000 || responses.empty())return;last_flush=now;
    bridge::ContactHeader h;h.session=session;h.generation=++sequence;h.snapshot=loaded;h.count=outgoing.size();
    auto tmp=responses;tmp+=".tmp";
    std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&h),sizeof h);
    out.write(reinterpret_cast<const char*>(outgoing.data()),outgoing.size()*sizeof(bridge::DynamicContact));out.close();
    #ifdef _WIN32
    const bool replaced=out && MoveFileExW(tmp.c_str(),responses.c_str(),MOVEFILE_REPLACE_EXISTING);
#else
    std::error_code ec;if(out)std::filesystem::rename(tmp,responses,ec);const bool replaced=out && !ec;
#endif
    if(replaced){outgoing.clear();waiting=h.generation;}
  }
};
Backend::Backend():impl_(std::make_unique<Impl>()){}
Backend::~Backend()=default;
std::string Backend::diagnostics() const {std::scoped_lock lock(impl_->mutex);return "proxies="+std::to_string(impl_->proxies.size())+" snapshots="+std::to_string(impl_->loaded)+" batches="+std::to_string(impl_->sequence);}
void Backend::clear(){std::scoped_lock lock(impl_->mutex);impl_->active=false;impl_->pending.reset();impl_->clear();}
void Backend::update(const std::filesystem::path& terrain,std::uint64_t session,bool active) {
  std::scoped_lock lock(impl_->mutex);
  if(impl_->session!=session){impl_->clear();impl_->pending.reset();impl_->session=session;impl_->sequence=0;}
  impl_->active=active;if(!active){impl_->pending.reset();return;}
  auto path=terrain;path.replace_extension(".rldyn");impl_->responses=terrain;impl_->responses.replace_extension(".rlcontacts");
  const auto now=bridge::now_us();if(now-impl_->last_read<8000)return;impl_->last_read=now;
  auto next=std::make_shared<Snapshot>();std::ifstream in(path,std::ios::binary);if(!in)return;
  in.read(reinterpret_cast<char*>(&next->header),sizeof next->header);const auto& h=next->header;
  if(!in || !bridge::valid(h) || h.session!=session || now<h.generation || now-h.generation>150000 || (impl_->pending && impl_->pending->header.generation==h.generation))return;
  next->bodies.resize(h.bodies);next->vertices.resize(h.vertices);
  in.read(reinterpret_cast<char*>(next->bodies.data()),next->bodies.size()*sizeof(bridge::DynamicBody));
  in.read(reinterpret_cast<char*>(next->vertices.data()),next->vertices.size()*sizeof(bridge::Vec3));
  if(!in || in.peek()!=EOF)return;
  std::unordered_set<std::uint64_t> ids;
  for(const auto& b:next->bodies)if(!bridge::valid(b,h.vertices)||!ids.insert(b.id).second)return;
  for(auto p:next->vertices)if(!bridge::valid(p)||bridge::length(p)>100000)return;
  impl_->pending=std::move(next);
}
void Backend::step(btRigidBody* car,btRigidBody* ball,float dt,btCollisionShape* terrain,std::uint64_t terrain_generation) {
  std::scoped_lock lock(impl_->mutex);const auto now=bridge::now_us();
  if(!impl_->active || !impl_->pending || now<impl_->pending->header.generation || now-impl_->pending->header.generation>150000 || !std::isfinite(dt)||dt<=0||dt>.05f){impl_->clear();return;}
  if(impl_->loaded!=impl_->pending->header.generation)impl_->synchronize(*impl_->pending);
  if(terrain && (!impl_->ground || impl_->ground_shape!=terrain || impl_->ground_generation!=terrain_generation)){
    if(impl_->ground)impl_->world.removeRigidBody(impl_->ground.get());
    btRigidBody::btRigidBodyConstructionInfo info(0,nullptr,terrain,{0,0,0});
    impl_->ground=std::make_unique<btRigidBody>(info);impl_->ground->setWorldTransform(btTransform::getIdentity());
    impl_->ground_shape=terrain;impl_->ground_generation=terrain_generation;impl_->world.addRigidBody(impl_->ground.get(),4,2);
  }
  if(impl_->ground)impl_->world.updateSingleAabb(impl_->ground.get());
  btRigidBody* native[]={car,ball};btVector3 before_v[2],before_w[2];btTransform before_xf[2];
  for(int i=0;i<2;++i){
    auto* source=native[i];
    if(!source){if(impl_->drivers[i]){impl_->world.removeRigidBody(impl_->drivers[i].get());impl_->drivers[i].reset();impl_->owned_shapes[i].reset();impl_->driver_shapes[i]=nullptr;impl_->driver_sources[i]=nullptr;}continue;}
    const float inv=source->getInvMass();if(!std::isfinite(inv)||inv<=0||inv>10)return;
    auto* shape=source->getCollisionShape();
    const auto lv=source->getLinearVelocity(),av=source->getAngularVelocity(),inertia=source->getInvInertiaDiagLocal();
    if(!bridge::valid(v(lv)) || !bridge::valid(v(av)) || !bridge::valid(v(inertia)) || lv.length()>1000 || av.length()>1000 || inertia.x()<0 || inertia.y()<0 || inertia.z()<0)return;
    const bool teleported=impl_->drivers[i] && (impl_->drivers[i]->getWorldTransform().getOrigin()-source->getWorldTransform().getOrigin()).length()>5.f;
    if(!impl_->drivers[i] || impl_->driver_shapes[i]!=shape || impl_->driver_sources[i]!=source || teleported){
      if(impl_->drivers[i])impl_->world.removeRigidBody(impl_->drivers[i].get());
      impl_->drivers[i].reset();impl_->owned_shapes[i].reset();
      auto owned=std::make_unique<DriverShape>();auto* copy=owned->clone(shape);
      if(!copy){impl_->driver_shapes[i]=nullptr;impl_->driver_sources[i]=nullptr;native[i]=nullptr;continue;}
      impl_->owned_shapes[i]=std::move(owned);
      btRigidBody::btRigidBodyConstructionInfo info(1/inv,nullptr,copy,{0,0,0});
      impl_->drivers[i]=std::make_unique<btRigidBody>(info);impl_->driver_shapes[i]=shape;impl_->driver_sources[i]=source;
      impl_->drivers[i]->setUserIndex(i);impl_->drivers[i]->setActivationState(DISABLE_DEACTIVATION);
      impl_->world.addRigidBody(impl_->drivers[i].get(),1,2);
    }
    auto& proxy=*impl_->drivers[i];proxy.setMassProps(1/inv,{0,0,0});proxy.setInvInertiaDiagLocal(source->getInvInertiaDiagLocal());
    before_xf[i]=source->getWorldTransform();before_v[i]=source->getLinearVelocity();before_w[i]=source->getAngularVelocity();
    proxy.setCenterOfMassTransform(before_xf[i]);proxy.setInterpolationWorldTransform(before_xf[i]);
    proxy.setLinearVelocity(before_v[i]);proxy.setAngularVelocity(before_w[i]);proxy.setFriction(source->getFriction());proxy.setRestitution(source->getRestitution());
    impl_->world.updateSingleAabb(&proxy);
  }
  // CCD-scale substeps prevent a fast ball crossing a small item in one frame.
  float speed=0;for(int i=0;i<2;++i)if(native[i])speed=std::max(speed,before_v[i].length());
  const int steps=std::clamp(static_cast<int>(std::ceil(speed*dt/.08f)),1,16);
  btVector3 integrated[2]={{0,0,0},{0,0,0}};
  for(int s=0;s<steps;++s){
    for(auto& [id,p]:impl_->proxies)if(p.record.mass==0){
      btTransform next;btTransformUtil::integrateTransform(p.body->getWorldTransform(),p.body->getLinearVelocity(),p.body->getAngularVelocity(),dt/steps,next);
      p.body->setWorldTransform(next);impl_->world.updateSingleAabb(p.body.get());
    }
    impl_->world.stepSimulation(dt/steps,0);
    for(int i=0;i<2;++i)if(native[i])integrated[i]+=impl_->drivers[i]->getLinearVelocity()*(dt/steps);
    for(int m=0;m<impl_->dispatcher.getNumManifolds();++m){
      auto* manifold=impl_->dispatcher.getManifoldByIndexInternal(m);
      const auto* a=static_cast<const btCollisionObject*>(manifold->getBody0());const auto* b=static_cast<const btCollisionObject*>(manifold->getBody1());
      int index=-1;bool a_is_driver=false;
      for(int i=0;i<2;++i){if(a==impl_->drivers[i].get()){index=i;a_is_driver=true;}else if(b==impl_->drivers[i].get()){index=i;a_is_driver=false;}}
      if(index<0)continue;const auto* target=a_is_driver?b:a;
      if(!target->getUserPointer())continue;
      const auto* p=static_cast<const Proxy*>(target->getUserPointer());
      for(int j=0;j<manifold->getNumContacts() && impl_->outgoing.size()<bridge::kContactLimit;++j){
        const auto& c=manifold->getContactPoint(j);if(c.getAppliedImpulse()<=1e-7f)continue;
        auto impulse=c.m_normalWorldOnB*c.getAppliedImpulse()+c.m_lateralFrictionDir1*c.m_appliedImpulseLateral1+c.m_lateralFrictionDir2*c.m_appliedImpulseLateral2;
        if(a_is_driver)impulse=-impulse;
        const auto point=a_is_driver?c.getPositionWorldOnB():c.getPositionWorldOnA();
        // Closing speed used only for gameplay damage; physical response comes
        // exclusively from the solver's complete contact manifold.
        const auto n=a_is_driver?-c.m_normalWorldOnB:c.m_normalWorldOnB;
        const auto closing=std::max(0.f,(before_v[index]+before_w[index].cross(point-before_xf[index].getOrigin())-v(p->record.state.velocity*(1/50.f))).dot(n)*50);
        impl_->outgoing.push_back({p->record.id,v(point*50- v(bridge::kNativeTerrainOffset)),v(impulse*50),closing,static_cast<std::uint32_t>(index)});
      }
    }
  }
  for(int i=0;i<2;++i)if(native[i]){
    auto& proxy=*impl_->drivers[i];
    // RL still performs all driving, terrain and car/ball integration. Feed back
    // only the contact velocity and split-impulse positional correction. Remove
    // free-flight integration performed by the coupling world's substeps.
    native[i]->setLinearVelocity(proxy.getLinearVelocity());native[i]->setAngularVelocity(proxy.getAngularVelocity());
    auto xf=before_xf[i];auto correction=proxy.getWorldTransform().getOrigin()-before_xf[i].getOrigin()-integrated[i];
    if(correction.length()<5.f){xf.setOrigin(xf.getOrigin()+correction);native[i]->setCenterOfMassTransform(xf);}
    native[i]->activate(true);
  }
  impl_->flush(now);
}
}
