#include "CarMenu.hpp"
#include "shared/car_menu.hpp"
#include <RE/U/UI.h>
#include <RE/U/UIMessageQueue.h>
#include <RE/S/SleepWaitMenu.h>
#include <RE/S/SendHUDMessage.h>
#include <RE/G/GFxMovieView.h>
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <string>
#include <array>
#include <algorithm>
namespace car_menu {
namespace {
bridge::car_menu::State state;
std::uint64_t pending_until{},last_input{};
RE::GFxValue clip;bool dirty{};
using Value=RE::GFxValue;
Value number(double v){return Value(v);}
void remove(){if(clip.IsObject())clip.Invoke("removeMovieClip");clip=Value{};}
void line(double x,double y,bool first=false){const std::array<Value,2> args{number(x),number(y)};clip.Invoke(first?"moveTo":"lineTo",nullptr,args.data(),args.size());}
void polygon(double cx,double cy,double inner,double outer,double start,double end,unsigned color){
  const std::array<Value,2> fill{number(color),number(94)};clip.Invoke("beginFill",nullptr,fill.data(),fill.size());
  constexpr int segments=16;
  for(int i=0;i<=segments;++i){const auto a=start+(end-start)*i/segments;line(cx+std::sin(a)*outer,cy-std::cos(a)*outer,i==0);}
  for(int i=segments;i>=0;--i){const auto a=start+(end-start)*i/segments;line(cx+std::sin(a)*inner,cy-std::cos(a)*inner);}
  line(cx+std::sin(start)*outer,cy-std::cos(start)*outer);clip.Invoke("endFill");
}
void text(const char* name,unsigned depth,const std::string& message,double x,double y,double width,unsigned size,unsigned color=0xffffff){
  const std::array<Value,6> args{Value(name),number(depth),number(x-width/2),number(y),number(width),number(size*2)};
  clip.Invoke("createTextField",nullptr,args.data(),args.size());
  Value field;if(!clip.GetMember(name,&field) || !field.IsObject())return;
  field.SetMember("html",Value(true));field.SetMember("selectable",Value(false));
  const auto html="<p align='center'><font face='$EverywhereFont' size='"+std::to_string(size)+"' color='#"+
    (color==0xffffff?std::string("FFFFFF"):std::string("F4B65D"))+"'>"+message+"</font></p>";
  field.SetMember("htmlText",Value(html.c_str()));
}
void draw(){
  if(!state.open){remove();return;}
  auto* ui=RE::UI::GetSingleton();auto hud=ui?ui->GetMenu("HUD Menu"):nullptr;
  auto* movie=hud && hud->uiMovie?hud->uiMovie.get():nullptr;if(!movie)return;
  remove();Value root;if(!movie->GetVariable(&root,"_root") || !root.IsObject())return;
  const std::array<Value,2> args{Value("SkyrimLeagueCarWheel"),number(24000)};
  if(!root.Invoke("createEmptyMovieClip",&clip,args.data(),args.size()) || !clip.IsObject()){
    spdlog::warn("Car wheel: HUD movie did not create the radial overlay");state.close();dirty=false;RE::SendHUDMessage::ShowHUDMessage("Car wheel could not display; see the bridge log");return;
  }
  const auto frame=movie->GetVisibleFrameRect();const double width=frame.right-frame.left,height=frame.bottom-frame.top;
  const double scale=std::min(width/1280,height/720),cx=(frame.left+frame.right)/2,cy=(frame.top+frame.bottom)/2;
  if(scale<=0)return;
  constexpr double tau=6.28318530718;
  for(unsigned i=0;i<8;++i){const auto center=i*tau/8;
    polygon(cx,cy,74*scale,238*scale,center-tau/16+.014,center+tau/16-.014,int(i)==state.selected?0x9a6328:0x171d24);
    const auto label=std::string(bridge::car_menu::labels[i]);const auto name="label"+std::to_string(i);
    text(name.c_str(),i+1,label,cx+std::sin(center)*164*scale,cy-std::cos(center)*164*scale-12*scale,138*scale,unsigned(19*scale));
  }
  polygon(cx,cy,0,68*scale,0,tau,0x10151b);
  text("title",20,"SKYRIM LEAGUE",cx,cy-20*scale,150*scale,unsigned(15*scale),0xf4b65d);
  text("hint",21,"Left stick / D-pad to choose  |  A / Cross: Select  |  B / Circle: Back",cx,cy+262*scale,900*scale,unsigned(18*scale));
  text("stick",22,"RS / R3: Close",cx,cy+294*scale,400*scale,unsigned(16*scale));
  dirty=false;
}
}
bool busy(){return state.open || pending_until>GetTickCount64();}
void close(){state.close();pending_until=0;dirty=false;remove();}
void native_opened(){close();}
void input(float x,float y,std::uint16_t buttons,bool blocked){
  last_input=GetTickCount64();const auto previous=state.selected;
  const auto action=state.input(x,y,buttons,blocked || pending_until>last_input);
  if(action==bridge::car_menu::Action::Open || previous!=state.selected)dirty=true;
  if(action==bridge::car_menu::Action::Close){remove();dirty=false;}
  if(action==bridge::car_menu::Action::Select){
    const auto selected=state.selected;remove();dirty=false;pending_until=last_input+1500;
    if(selected==6)RE::SleepWaitMenu::ToggleOpenMenu(false);
    else if(auto* queue=RE::UIMessageQueue::GetSingleton())queue->AddMessage(RE::BSFixedString(bridge::car_menu::menus[selected]),RE::UI_MESSAGE_TYPE::kShow,nullptr);
    spdlog::info("Car wheel selected {}",bridge::car_menu::labels[selected]);
  }
}
void tick(){
  const auto now=GetTickCount64();
  if(state.open && now-last_input>1500){close();return;}
  if(pending_until && now>=pending_until){pending_until=0;RE::SendHUDMessage::ShowHUDMessage("That menu could not open here");}
  if(dirty)draw();
}
}
