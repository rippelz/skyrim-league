#pragma once
#include "shared/transform.hpp"
#include <filesystem>
namespace RE {class PlayerCharacter;}
namespace terrain_export {
bool dynamic(RE::PlayerCharacter* player,const bridge::Transform& transform,std::uint64_t session,std::uint64_t now,const std::filesystem::path& path,float damage_scale,unsigned cooldown);
bool write(RE::PlayerCharacter* player,const bridge::Transform& transform,std::uint64_t session,std::uint64_t generation,const std::filesystem::path& path);
}
