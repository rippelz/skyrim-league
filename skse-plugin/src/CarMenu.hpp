#pragma once
#include <cstdint>
namespace car_menu {
bool busy();
void input(float x,float y,std::uint16_t buttons,bool blocked);
void tick();
void close();
void native_opened();
}
