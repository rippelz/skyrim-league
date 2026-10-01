#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include "shared/protocol.hpp"
namespace native_terrain {
class Backend {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  Backend();~Backend();
  bool initialize(std::function<void(std::string)> log);
  void update(const std::filesystem::path& file,std::uint64_t session,bridge::Vec3 car,bool active,bridge::Vec3 ball={},bool dynamic=true);
  bool applied() const;
  void shutdown();
};
}
