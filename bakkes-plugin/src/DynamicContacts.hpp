#pragma once
#include "shared/dynamic_contacts.hpp"
#include <filesystem>
#include <functional>
#include <memory>
class btRigidBody;
class btCollisionShape;
namespace dynamic_contacts {
class Backend {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  Backend();~Backend();
  void update(const std::filesystem::path& terrain,std::uint64_t session,bool active);
  // Called on RL's physics thread, immediately before its native world step.
  void step(btRigidBody* car,btRigidBody* ball,float dt,btCollisionShape* terrain=nullptr,std::uint64_t terrain_generation=0);
  void clear();
  std::string diagnostics() const;
};
}
