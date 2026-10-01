#include "bakkes-plugin/src/DynamicContacts.hpp"
#include "shared/udp.hpp"
#include "shared/terrain_mesh.hpp"
#include <btBulletDynamicsCommon.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
using namespace bridge;
void require(bool condition,const char* msg){if(!condition)throw std::runtime_error(msg);}
struct Result {float velocity,angular;Vec3 impulse;float closing;};
Result hit(float speed,float lateral,float mass,float x=72,float target_velocity=0,float height=0) {
  auto root=std::filesystem::temp_directory_path()/("skyrim-league-contact-"+std::to_string(now_us()));std::filesystem::create_directory(root);
  auto terrain=root/"scene.rltm",path=root/"scene.rldyn",response=root/"scene.rlcontacts";
  DynamicHeader h;h.session=42;h.generation=now_us();h.bodies=1;h.vertices=8;
  DynamicBody target;target.id=7;target.shape=8;target.state.position={x,lateral,height};target.mass=mass;target.state.velocity={target_velocity,0,0};target.count=8;target.friction=0;target.restitution=.25f;
  std::vector<Vec3> points;for(int i=0;i<8;++i)points.push_back({(i&1)?20.f:-20.f,(i&2)?20.f:-20.f,(i&4)?20.f:-20.f});
  {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&h),sizeof h);out.write(reinterpret_cast<char*>(&target),sizeof target);out.write(reinterpret_cast<char*>(points.data()),points.size()*sizeof(Vec3));}
  btBoxShape shape({1,1,1});btVector3 inertia;shape.calculateLocalInertia(180,inertia);btRigidBody::btRigidBodyConstructionInfo info(180,nullptr,&shape,inertia);btRigidBody car(info);
  car.setWorldTransform(btTransform(btQuaternion(0,0,0,1),{0,0,(height+kNativeTerrainOffset.z)/50}));car.setLinearVelocity({speed/50,0,0});car.setRestitution(.25f);car.setFriction(0);
  Result result{};
  {
    dynamic_contacts::Backend backend;backend.update(terrain,42,true);backend.step(&car,nullptr,.016f);
    result.velocity=car.getLinearVelocity().x()*50;result.angular=car.getAngularVelocity().z();
    ContactHeader contacts;std::ifstream in(response,std::ios::binary);in.read(reinterpret_cast<char*>(&contacts),sizeof contacts);if(!(in && valid(contacts) && contacts.count)){std::cerr<<"Missing contact speed="<<speed<<" mass="<<mass<<" x="<<x<<" "<<backend.diagnostics()<<" velocity="<<result.velocity<<"\n";}require(in && valid(contacts) && contacts.count,"solver produced no contacts");
    for(unsigned i=0;i<contacts.count;++i){DynamicContact c;in.read(reinterpret_cast<char*>(&c),sizeof c);require(in && valid(c),"invalid solved contact");require(std::abs(c.point.z-height)<60,"contact feedback retained native height offset");result.impulse=result.impulse+c.impulse;result.closing=std::max(result.closing,c.closing);}
    // Missing/new-session/stale snapshots must not continue applying contacts.
    backend.update(terrain,43,true);const auto before=car.getLinearVelocity();backend.step(&car,nullptr,.016f);require((car.getLinearVelocity()-before).length()<1e-6,"retired session affected car");
  }
  std::filesystem::remove_all(root);return result;
}

void reset_ball() {
  auto root=std::filesystem::temp_directory_path()/("skyrim-league-reset-"+std::to_string(now_us()));std::filesystem::create_directory(root);
  auto terrain=root/"scene.rltm",path=root/"scene.rldyn";
  DynamicHeader h;h.session=42;h.generation=now_us();h.bodies=1;h.vertices=8;
  DynamicBody target;target.id=7;target.shape=8;target.state.position={72,0,0};target.mass=80;target.count=8;
  std::vector<Vec3> points;for(int i=0;i<8;++i)points.push_back({(i&1)?20.f:-20.f,(i&2)?20.f:-20.f,(i&4)?20.f:-20.f});
  {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&h),sizeof h);out.write(reinterpret_cast<char*>(&target),sizeof target);out.write(reinterpret_cast<char*>(points.data()),points.size()*sizeof(Vec3));}
  btBoxShape car_shape({1,1,1});btVector3 inertia;car_shape.calculateLocalInertia(180,inertia);
  btRigidBody::btRigidBodyConstructionInfo car_info(180,nullptr,&car_shape,inertia);btRigidBody car(car_info);car.setWorldTransform(btTransform(btQuaternion(0,0,0,1),{-50,0,kNativeTerrainOffset.z/50}));
  {
    dynamic_contacts::Backend backend;backend.update(terrain,42,true);
    std::unique_ptr<btSphereShape> sphere;std::unique_ptr<btRigidBody> ball;
    for(int i=0;i<100;++i){
      std::this_thread::sleep_for(std::chrono::milliseconds(9));
      h.generation=now_us();
      {ContactHeader previous{};std::ifstream in(root/"scene.rlcontacts",std::ios::binary);in.read(reinterpret_cast<char*>(&previous),sizeof previous);if(in && valid(previous))h.ack=previous.generation;}
      {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&h),sizeof h);out.write(reinterpret_cast<char*>(&target),sizeof target);out.write(reinterpret_cast<char*>(points.data()),points.size()*sizeof(Vec3));}
      backend.update(terrain,42,true);
      sphere=std::make_unique<btSphereShape>(1.1f);sphere->calculateLocalInertia(30,inertia);
      btRigidBody::btRigidBodyConstructionInfo info(30,nullptr,sphere.get(),inertia);ball=std::make_unique<btRigidBody>(info);
      ball->setWorldTransform(btTransform(btQuaternion(0,0,0,1),{0,0,kNativeTerrainOffset.z/50}));ball->setLinearVelocity({12,0,0});
      backend.step(&car,ball.get(),.016f);
      require(ball->getLinearVelocity().x()<11.999f,"reset cycle skipped live ball contact");
      const btTransform teleported_pose(btQuaternion(0,0,0,1),{50,0,kNativeTerrainOffset.z/50});ball->setCenterOfMassTransform(teleported_pose);ball->setLinearVelocity({0,0,0});ball->setAngularVelocity({0,0,0});
      backend.step(&car,ball.get(),.016f);
      require((ball->getWorldTransform().getOrigin()-teleported_pose.getOrigin()).length()<1e-4,"ball teleport retained a stale contact correction");
      // Engine-owned body/shape die before the next physics observation. The
      // coupling world must not retain either object through that reset gap.
      ball.reset();sphere.reset();backend.step(&car,nullptr,.016f);
    }
  }
  std::filesystem::remove_all(root);
}

int main(){try {
  reset_ball();
  auto centered=hit(600,0,80);require(centered.velocity<599,"car did not slow against movable body");require(centered.impulse.x>0,"movable body got no forward impulse");
  auto deep=hit(600,0,80,72,0,-75000);require(std::abs(deep.velocity-centered.velocity)<.1f,"deep descent changed moving-body contact response");
  require(std::abs(180*(centered.velocity-600)+centered.impulse.x)<.1f,"contact did not conserve linear momentum");
  auto offset=hit(600,35,80);require(std::abs(offset.angular)>.01f,"off-center impact did not rotate car");
  auto gentle=hit(5,0,80,70);require(gentle.velocity<5 && gentle.impulse.x>0,"low-speed pushing was filtered by damage threshold");
  auto heavy=hit(600,0,800);require(heavy.velocity<centered.velocity,"heavier object did not resist more");
  auto anchored=hit(600,0,0);require(anchored.velocity<heavy.velocity,"kinematic object did not block car");
  auto approaching=hit(600,0,0,72,-300);require(approaching.velocity<anchored.velocity,"moving kinematic object velocity did not affect contact");
  DynamicHeader h;h.session=1;h.generation=1;h.bodies=kDynamicLimit+1;require(!valid(h),"oversize body stream accepted");
  DynamicBody b;b.id=1;b.shape=1;b.count=4;require(valid(b,4),"valid body rejected");b.first=0xffffffff;require(!valid(b,4),"vertex range overflow accepted");
  std::cout<<"Contact momentum, torque, mass response, blocking, session rejection and 100 ball-reset/teleport cycles passed\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
