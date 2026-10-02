#pragma once
#include <cmath>
#include <cstdint>
#include <array>
#include <string_view>
namespace bridge::car_menu {
constexpr std::array<std::string_view,8> labels{"Inventory","Magic","Map","Skills","Journal","Favorites","Wait","Skyrim Menu"};
constexpr std::array<std::string_view,8> menus{"InventoryMenu","MagicMenu","MapMenu","StatsMenu","Journal Menu","FavoritesMenu","Sleep/Wait Menu","TweenMenu"};
constexpr std::uint16_t right_click=0x80,confirm=0x1000,cancel=0x2000;
enum class Action { None, Open, Close, Select };
struct State {
  bool open{};int selected=-1;std::uint16_t previous{};
  Action input(float x,float y,std::uint16_t buttons,bool blocked=false) {
    const auto pressed=std::uint16_t(buttons&~previous);previous=buttons;
    if(blocked){const bool was=open;open=false;selected=-1;return was?Action::Close:Action::None;}
    if(pressed&right_click){open=!open;selected=-1;return open?Action::Open:Action::Close;}
    if(!open)return Action::None;
    if(pressed&cancel){open=false;return Action::Close;}
    if(buttons&15){x=float(bool(buttons&8))-float(bool(buttons&4));y=float(bool(buttons&1))-float(bool(buttons&2));}
    if(std::isfinite(x) && std::isfinite(y) && x*x+y*y>=.35f*.35f){
      constexpr float tau=6.28318530718f;
      auto angle=std::atan2(x,y);if(angle<0)angle+=tau;
      selected=int(std::floor(angle/(tau/8)+.5f))%8;
    }
    if((pressed&confirm) && selected>=0){open=false;return Action::Select;}
    return Action::None;
  }
  void close(){open=false;selected=-1;}
};
}
