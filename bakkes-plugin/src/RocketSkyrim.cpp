#include "shared/udp.hpp" // Winsock must precede Windows headers
#include "shared/protocol.hpp"
#include "shared/transform.hpp"
#include "shared/terrain_mesh.hpp"
#include "NativeTerrain.hpp"
#include <typeindex>
#include <fstream>
#include <random>
#include <Xinput.h>
#include "bakkesmod/wrappers/SettingsWrapper.h"
#include "bakkesmod/wrappers/PlayerControllerWrapper.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/wrappers/GameObject/CarComponent/BoostWrapper.h"
#include "bakkesmod/wrappers/gfx/GfxDataTrainingWrapper.h"
#include "bakkesmod/wrappers/items/ItemsWrapper.h"
#include "bakkesmod/wrappers/items/ProductWrapper.h"
#include "bakkesmod/wrappers/GameObject/CarComponent/PrimitiveComponentWrapper.h"
#include "bakkesmod/wrappers/GameObject/CarComponent/VehicleSimWrapper.h"
#include "bakkesmod/wrappers/GameObject/CarComponent/WheelWrapper.h"
#include "bakkesmod/wrappers/ArrayWrapper.h"

namespace {
constexpr auto kTick = "Function TAGame.Car_TA.SetVehicleInput";
bridge::Vec3 vec(Vector v) { return {v.X,v.Y,v.Z}; }
Vector vec(bridge::Vec3 v) { return {v.x,v.y,v.z}; }
bridge::Quat bridge_quat(Quat q) { return {q.X,q.Y,q.Z,q.W}; }
Quat sdk_quat(bridge::Quat q) { return {q.w,q.x,q.y,q.z}; } // SDK ctor: W, X, Y, Z
bridge::Body body(RBActorWrapper actor) {
  const auto rb=actor.GetCurrentRBState();
  return {vec(actor.GetLocation()),bridge_quat(rb.Quaternion),vec(actor.GetVelocity()),vec(actor.GetAngularVelocity())};
}
}
class RocketSkyrim : public BakkesMod::Plugin::BakkesModPlugin {
  bridge::Udp socket_;
  native_terrain::Backend native_terrain_;
  std::uint64_t session_{};
  std::uint32_t sequence_{};
  std::uint64_t last_event_time_{};
  bool have_event_{},interaction_pending_{},driving_paused_{};
  RBState paused_car_{},paused_ball_{};
  std::uint64_t interaction_hold_{};
  void pause_driving(CarWrapper car,bool pause) {
    if(car.IsNull())return;
    auto server=gameWrapper->GetCurrentGameState();auto ball=server.IsNull()?BallWrapper(0):server.GetBall();
    if(pause && !driving_paused_){paused_car_=car.GetCurrentRBState();if(!ball.IsNull())paused_ball_=ball.GetCurrentRBState();}
    if(pause!=driving_paused_){car.SetFrozen(pause);if(!ball.IsNull())ball.SetFrozen(pause);
      if(!pause){car.SetPhysicsState(paused_car_);if(!ball.IsNull())ball.SetPhysicsState(paused_ball_);}
      driving_paused_=pause;cvarManager->log(std::string("RocketSkyrim menu physics ")+(pause?"paused":"resumed"));}
  }
  using PollPad=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
  HMODULE pad_module_{};
  PollPad poll_pad_{};
  bool background_input_{};
  float background_look_right_{};
  std::uint64_t swivel_log_{};
  WORD previous_buttons_{};
  bridge::Vec3 menu_pad_{};bool menu_pad_connected_{};
  std::uint64_t input_log_{},binding_refresh_{};
  std::vector<std::pair<std::string,std::string>> pad_bindings_;
  GamepadSettings pad_settings_{};
  bool binding_logged_{};
  bool freeplay_was_[5]{};
  void background_controller(CarWrapper car) {
    // Native Windows sees both games in one desktop; Proton prefixes retain
    // the focus-file fallback supplied by the Linux launcher.
    bool skyrim_focused=false;
    static const bool native_windows=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")==nullptr;
    DWORD focused_pid{};if(native_windows)GetWindowThreadProcessId(GetForegroundWindow(),&focused_pid);
    if(native_windows) if(auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,focused_pid)) {
      wchar_t image[32768]{};DWORD size=32768;
      if(QueryFullProcessImageNameW(process,0,image,&size)) {
        const auto* name=wcsrchr(image,L'\\');
        skyrim_focused=_wcsicmp(name?name+1:image,L"SkyrimSE.exe")==0;
      }
      CloseHandle(process);
    }
    try {const auto path=gameWrapper->GetDataFolder()/"rocket-skyrim-focus.txt";
      if(std::filesystem::file_time_type::clock::now()-std::filesystem::last_write_time(path)<std::chrono::seconds(2)){
        std::ifstream f(path);char value{};f.get(value);skyrim_focused=skyrim_focused || value=='1';}}
    catch(const std::filesystem::filesystem_error&){}
    const bool active=skyrim_focused && !driving_paused_ &&
      bridge::now_us()-last_feedback_<3000000 && native_terrain_.applied();
    if(active!=background_input_){background_input_=active;if(!active)background_look_right_=0;
      cvarManager->log(std::string("RocketSkyrim background controller ")+(active?"enabled":"disabled"));}
    menu_pad_connected_=false;
    if(!poll_pad_ || !native_terrain_.applied())return;
    XINPUT_STATE state{};DWORD status=ERROR_DEVICE_NOT_CONNECTED;int index=-1;
    for(DWORD i=0;i<4;++i)if((status=poll_pad_(i,&state))==ERROR_SUCCESS){index=static_cast<int>(i);break;}
    if(bridge::now_us()-input_log_>1000000){input_log_=bridge::now_us();
      cvarManager->log("RocketSkyrim background pad="+std::to_string(index)+" buttons="+std::to_string(state.Gamepad.wButtons)+
        " RT="+std::to_string(state.Gamepad.bRightTrigger)+" LT="+std::to_string(state.Gamepad.bLeftTrigger)+" paused="+std::to_string(driving_paused_)+" routing="+std::to_string(active));}
    menu_pad_connected_=status==ERROR_SUCCESS;
    if(menu_pad_connected_)menu_pad_={std::clamp(state.Gamepad.sThumbLX/32767.f,-1.f,1.f),std::clamp(state.Gamepad.sThumbLY/32767.f,-1.f,1.f),float(state.Gamepad.wButtons)};
    auto pc=gameWrapper->GetPlayerController();if(pc.IsNull())return;
    if(bridge::now_us()-binding_refresh_>2000000){binding_refresh_=bridge::now_us();auto settings=gameWrapper->GetSettings();
      pad_bindings_=settings.GetAllGamepadBindings();pad_settings_=settings.GetGamepadSettings();
      if(!binding_logged_){for(const auto& b:pad_bindings_)cvarManager->log("RocketSkyrim pad binding "+b.first+" = "+b.second);binding_logged_=true;}}
    const bool interact_pressed=(state.Gamepad.wButtons&XINPUT_GAMEPAD_B) && !(previous_buttons_&XINPUT_GAMEPAD_B);
    previous_buttons_=state.Gamepad.wButtons;
    DWORD foreground_pid{};GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);
    if(interact_pressed && !driving_paused_ && bridge::now_us()-last_feedback_<3000000 &&
       (active || foreground_pid==GetCurrentProcessId())){interaction_pending_=true;interaction_hold_=0;}
    if(!active)return;
    auto& pad=state.Gamepad;
    auto axis=[&](SHORT raw){const float v=std::clamp(raw/32767.0f,-1.0f,1.0f);const float dead=std::clamp(pad_settings_.ControllerDeadzone,0.0f,.95f);
      return std::abs(v)<=dead?0.0f:std::copysign((std::abs(v)-dead)/(1-dead),v);};
    auto key=[&](const std::string& name)->float {
      if(name=="XboxTypeS_LeftX")return axis(pad.sThumbLX);if(name=="XboxTypeS_LeftY")return axis(pad.sThumbLY);
      if(name=="XboxTypeS_RightX")return axis(pad.sThumbRX);if(name=="XboxTypeS_RightY")return axis(pad.sThumbRY);
      if(name=="XboxTypeS_RightTriggerAxis")return pad.bRightTrigger/255.0f;if(name=="XboxTypeS_LeftTriggerAxis")return pad.bLeftTrigger/255.0f;
      if(name=="XboxTypeS_RightTrigger")return pad.bRightTrigger>30;if(name=="XboxTypeS_LeftTrigger")return pad.bLeftTrigger>30;
      const std::pair<const char*,WORD> buttons[]={{"XboxTypeS_A",XINPUT_GAMEPAD_A},{"XboxTypeS_B",XINPUT_GAMEPAD_B},{"XboxTypeS_X",XINPUT_GAMEPAD_X},{"XboxTypeS_Y",XINPUT_GAMEPAD_Y},
        {"XboxTypeS_DPad_Up",XINPUT_GAMEPAD_DPAD_UP},{"XboxTypeS_DPad_Down",XINPUT_GAMEPAD_DPAD_DOWN},{"XboxTypeS_DPad_Left",XINPUT_GAMEPAD_DPAD_LEFT},{"XboxTypeS_DPad_Right",XINPUT_GAMEPAD_DPAD_RIGHT},{"XboxTypeS_LeftShoulder",XINPUT_GAMEPAD_LEFT_SHOULDER},{"XboxTypeS_RightShoulder",XINPUT_GAMEPAD_RIGHT_SHOULDER},{"XboxTypeS_LeftThumbstick",XINPUT_GAMEPAD_LEFT_THUMB},{"XboxTypeS_RightThumbstick",XINPUT_GAMEPAD_RIGHT_THUMB}};
      for(const auto& b:buttons)if(name==b.first)return (pad.wButtons&b.second)!=0;return 0;
    };
    auto action=[&](const char* name){float value=0;for(const auto& b:pad_bindings_){
      if(b.first==name)value=std::max(value,key(b.second));else if(b.second==name)value=std::max(value,key(b.first));}return value;};
    const char* freeplay[]={"FreeplayBallInFront","FreeplayBallOnCar","FreeplayRedirectPass","FreeplayPopBallUp","FreeplayDefendShot"};
    for(int i=0;i<5;++i){const bool pressed=action(freeplay[i])>0;
      if(pressed && !freeplay_was_[i]){
        auto server=gameWrapper->GetCurrentGameState();auto ball=server.IsNull()?BallWrapper(0):server.GetBall();
        if(!ball.IsNull()){
          const auto before=ball.GetCurrentRBState();gameWrapper->ExecuteUnrealCommand(freeplay[i]);const auto after=ball.GetCurrentRBState();
          // Older UE builds do not expose every action as a console exec. Keep
          // the native ball body usable outside the stock arena in that case.
          if(bridge::length(vec(after.Location)-vec(before.Location))<.01f && bridge::length(vec(after.LinearVelocity)-vec(before.LinearVelocity))<.01f){
            auto rb=before;const auto car_state=car.GetCurrentRBState();auto forward=bridge::rotate(bridge_quat(car_state.Quaternion),{1,0,0});
            forward.z=0;const auto length=bridge::length(forward);forward=length>.001f?forward*(1/length):bridge::Vec3{1,0,0};
            const auto car_position=vec(car_state.Location);
            if(i==0){rb.Location=vec(car_position+forward*220+bridge::Vec3{0,0,100});rb.LinearVelocity=car_state.LinearVelocity;}
            if(i==1){rb.Location=vec(car_position+bridge::Vec3{0,0,145});rb.LinearVelocity=car_state.LinearVelocity;}
            if(i==2){const auto delta=car_position+bridge::Vec3{0,0,100}-vec(rb.Location);const auto distance=bridge::length(delta);if(distance>1)rb.LinearVelocity=vec(delta*(1400/distance));}
            if(i==3){rb.LinearVelocity=vec(vec(rb.LinearVelocity)+bridge::Vec3{0,0,1000});}
            if(i==4){rb.Location=vec(car_position+forward*2200+bridge::Vec3{0,0,300});rb.LinearVelocity=vec(forward*-1600+bridge::Vec3{0,0,-120});}
            rb.AngularVelocity={0,0,0};ball.SetPhysicsState(rb);ball.SetLocation(rb.Location);ball.SetVelocity(rb.LinearVelocity);
          }
          cvarManager->log(std::string("RocketSkyrim background ball action ")+freeplay[i]);
        }
      }freeplay_was_[i]=pressed;
    }
    ControllerInput input{};
    input.Throttle=action("ThrottleForward")-action("ThrottleReverse");
    const auto steering=std::max(.01f,pad_settings_.SteeringSensitivity),air=std::max(.01f,pad_settings_.AirControlSensitivity);
    input.Steer=std::clamp(axis(pad.sThumbLX)*steering,-1.0f,1.0f);
    input.Pitch=std::clamp(-axis(pad.sThumbLY)*air,-1.0f,1.0f);
    input.Yaw=std::clamp(axis(pad.sThumbLX)*air,-1.0f,1.0f);
    input.Roll=std::clamp(action("RollRight")-action("RollLeft"),-1.0f,1.0f);
    if(action("ToggleRoll")>0){input.Roll=input.Yaw;input.Yaw=0;}
    input.DodgeForward=axis(pad.sThumbLY);input.DodgeStrafe=axis(pad.sThumbLX);
    input.Jump=action("Jump")>0;input.Handbrake=action("Handbrake")>0;
    input.ActivateBoost=input.HoldingBoost=action("Boost")>0;
    pc.SetVehicleInput(input);car.SetInput(input);
    pc.SetALookUp(axis(pad.sThumbRY));background_look_right_=axis(pad.sThumbRX);
    // Use the game's camera actions and the bridge's existing interaction route.
    const bool camera=action("SecondaryCamera")>0;
    // Track action edges separately so custom camera bindings work too.
    static bool camera_was=false,rear_was=false;
    if(camera&&!camera_was)pc.PressSecondaryCamera();if(!camera&&camera_was)pc.ReleaseSecondaryCamera();camera_was=camera;
    const bool rear=action("RearCamera")>0;
    if(rear&&!rear_was)pc.PressRearCamera();if(!rear&&rear_was)pc.ReleaseRearCamera();rear_was=rear;
  }
  int logged_body_=-1;
  std::uint64_t last_contact_log_{};
  std::ofstream recording_;
  std::uint64_t record_start_{}, last_log_{};
  std::uint64_t last_feedback_{};
  std::uintptr_t collision_car_{},collision_ball_{};
  unsigned long car_world_{},ball_world_{},car_bounds_{},ball_bounds_{};
  static void shift_body(RBActorWrapper actor,bridge::Vec3 delta) {
    auto state=actor.GetCurrentRBState();state.Location=vec(vec(state.Location)+delta);
    actor.SetPhysicsState(state);actor.SetLocation(state.Location);
  }
  bridge::Vec3 physics_offset_{},resume_position_{};
  bool resume_pending_{};
  std::uint64_t resume_session_{};std::uint32_t resume_sequence_{};
  bridge::Vec3 previous_physical_{},last_ground_position_{};
  bool have_previous_physical_{},have_ground_position_{};
  std::uintptr_t offset_car_{},offset_ball_{};
  unsigned long offset_car_bounds_{},offset_ball_bounds_{};
  void physics_origin(CarWrapper car) {
    const auto next=native_terrain_.applied()?bridge::kNativeTerrainOffset:bridge::Vec3{};
    auto server=gameWrapper->GetCurrentGameState();if(server.IsNull())return;
    auto ball=server.GetBall();
    const auto delta=next-physics_offset_;
    if(car.memory_address!=offset_car_ && next.z) {offset_car_=car.memory_address;offset_car_bounds_=collision_car_==car.memory_address?car_bounds_:car.GetbPhysRigidBodyOutOfWorldCheck();shift_body(car,next);}
    else if(delta.z && car.memory_address==offset_car_)shift_body(car,delta);
    if(!ball.IsNull()) {
      if(ball.memory_address!=offset_ball_ && next.z) {offset_ball_=ball.memory_address;offset_ball_bounds_=collision_ball_==ball.memory_address?ball_bounds_:ball.GetbPhysRigidBodyOutOfWorldCheck();shift_body(ball,next);}
      else if(delta.z && ball.memory_address==offset_ball_)shift_body(ball,delta);
      if(next.z)ball.SetbPhysRigidBodyOutOfWorldCheck(false);
      else if(ball.memory_address==offset_ball_)ball.SetbPhysRigidBodyOutOfWorldCheck(offset_ball_bounds_);
    }
    if(next.z)car.SetbPhysRigidBodyOutOfWorldCheck(false);
    else if(car.memory_address==offset_car_)car.SetbPhysRigidBodyOutOfWorldCheck(offset_car_bounds_);
    if(delta.z)cvarManager->log("RocketSkyrim native physics height offset="+std::to_string(next.z));
    if(next.z && resume_pending_){auto state=car.GetCurrentRBState();state.Location=vec(resume_position_+next);state.LinearVelocity={0,0,0};state.AngularVelocity={0,0,0};
      car.SetPhysicsState(state);car.SetLocation(state.Location);car.SetVelocity({0,0,0});resume_pending_=false;have_previous_physical_=false;
      cvarManager->log("RocketSkyrim restored car position after plugin reload");}
    physics_offset_=next;if(!next.z){offset_car_=offset_ball_=0;have_previous_physical_=false;have_ground_position_=false;}
    else {
      auto physical=vec(car.GetLocation());
      // Offline freeplay reset teleports to an unshifted arena spawn. Put the car
      // back at its last terrain rest point instead of far below the new terrain.
      if(have_previous_physical_ && !delta.z && bridge::length(physical-previous_physical_)>2000 &&
          std::abs(physical.x)<4100 && std::abs(physical.y)<5200 && std::abs(physical.z)<300) {
        const auto target=(have_ground_position_?last_ground_position_:bridge::Vec3{physical.x,physical.y,17})+next;
        shift_body(car,target-physical);car.SetVelocity({0,0,0});physical=target;
      }
      if(car.IsOnGround()){last_ground_position_=physical-next;have_ground_position_=true;}
      previous_physical_=physical;have_previous_physical_=true;
    }
  }
  WheelContactData wheel_template_[4]{};
  bridge::EventPacket wheel_hit_[4]{};
  std::uint64_t wheel_time_[4]{};
  void wheels(CarWrapper car,const bridge::StatePacket& state) {
    auto sim=car.GetVehicleSim();if(sim.IsNull())return;
    auto wheels=sim.GetWheels();if(wheels.Count()!=4)return;
    for(int i=0;i<4;++i) {auto wheel=wheels.Get(i);if(!wheel.IsNull()) {auto contact=wheel.GetContact();if(contact.bHasContact)wheel_template_[i]=contact;}}
    if (!cvarManager->getCvar("sb_wheel_contacts").getBoolValue() || bridge::now_us()-last_feedback_>500000) return;
    int grounded=0;
    for(int i=0;i<4;++i) {
      auto wheel=wheels.Get(i);if(wheel.IsNull())continue;
      auto original=wheel.GetContact();if(original.PhysMatProp && original.bHasContact)wheel_template_[i]=original;
      bridge::EventPacket probe{};probe.header=state.header;probe.header.kind=bridge::Kind::Event;probe.header.bytes=sizeof probe;probe.target_sequence=state.header.sequence;probe.type=static_cast<bridge::EventType>(7+i);
      probe.position=state.car.position+bridge::rotate(state.car.rotation,vec(wheel.GetLocalSuspensionRayStart()));
      probe.velocity=bridge::rotate(state.car.rotation,{0,0,-(wheel.GetSuspensionTravel()+wheel.GetSuspensionMaxRaise()+wheel.GetWheelRadius()+25)});
      if(bridge::valid(probe))socket_.send(&probe,sizeof probe,static_cast<std::uint16_t>(cvarManager->getCvar("sb_state_port").getIntValue()));
      if(bridge::now_us()-wheel_time_[i]>100000)continue;
      auto contact=wheel_template_[i];const auto& hit=wheel_hit_[i];
      contact.bHasContact=static_cast<unsigned>(hit.type)<11;contact.bHasContactWithWorldGeometry=contact.bHasContact;
      if(contact.bHasContact) {
        if(!contact.PhysMatProp)continue;
        contact.Location=vec(hit.position);contact.Normal=vec(hit.velocity);
        // Use the wheel's steering angle and the surface tangent, rather than
        // giving every wheel an unsteered car-axis friction direction.
        const auto steer=wheel.GetSteer2();
        auto forward=bridge::rotate(state.car.rotation,{std::cos(steer),std::sin(steer),0});
        const auto normal=hit.velocity;
        const auto along=forward.x*normal.x+forward.y*normal.y+forward.z*normal.z;
        forward=forward-normal*along;
        const auto magnitude=bridge::length(forward);
        if(magnitude<0.001f)continue;
        forward=forward*(1.0f/magnitude);
        const bridge::Vec3 right{normal.y*forward.z-normal.z*forward.y,normal.z*forward.x-normal.x*forward.z,normal.x*forward.y-normal.y*forward.x};
        contact.LongDirection=vec(forward);contact.LatDirection=vec(right);
      }
      wheel.SetContact(contact);wheel.SetbHadContact(contact.bHasContact);
      grounded+=contact.bHasContact;
    }
    car.SetbOnGround(grounded>=2);
  }
  void map_collision(RBActorWrapper actor,bool enabled) {
    auto state=actor.GetCurrentRBState();
    actor.SetbCollideWorld(enabled);
    actor.SetbPhysRigidBodyOutOfWorldCheck(enabled);
    auto component=actor.GetCollisionComponent();
    if(!component.IsNull())for(unsigned char channel:{0,8,9,18})component.SetRBCollidesWithChannel(channel,enabled);
    actor.ReInitRBPhys();actor.SetPhysicsState(state);
  }
  void noclip(CarWrapper car) {
    const bool active=cvarManager->getCvar("sb_enabled").getBoolValue() && !native_terrain_.applied() && cvarManager->getCvar("sb_noclip").getBoolValue() && bridge::now_us()-last_feedback_<500000;
    auto server=gameWrapper->GetCurrentGameState();if(server.IsNull())return;
    auto ball=server.GetBall();
    if(native_terrain_.applied()) {car.SetbPhysRigidBodyOutOfWorldCheck(false);if(!ball.IsNull())ball.SetbPhysRigidBodyOutOfWorldCheck(false);}
    if(active) {
      if(collision_car_!=car.memory_address) {collision_car_=car.memory_address;car_world_=car.GetbCollideWorld();car_bounds_=car.GetbPhysRigidBodyOutOfWorldCheck();map_collision(car,false);cvarManager->log("RocketSkyrim requested arena noclip; rebuilt car physics body");}
      if(!ball.IsNull() && collision_ball_!=ball.memory_address) {collision_ball_=ball.memory_address;ball_world_=ball.GetbCollideWorld();ball_bounds_=ball.GetbPhysRigidBodyOutOfWorldCheck();map_collision(ball,false);}
    } else {
      if(collision_car_==car.memory_address) {map_collision(car,true);car.SetbCollideWorld(car_world_);car.SetbPhysRigidBodyOutOfWorldCheck(native_terrain_.applied()?false:car_bounds_);}
      if(!ball.IsNull() && collision_ball_==ball.memory_address) {map_collision(ball,true);ball.SetbCollideWorld(ball_world_);ball.SetbPhysRigidBodyOutOfWorldCheck(native_terrain_.applied()?false:ball_bounds_);}
      collision_car_=collision_ball_=0;
    }
  }
  void surface(RBActorWrapper actor,const bridge::EventPacket& e,bool ball) {
    if(native_terrain_.applied())return; // native contacts own the car/ball response
    using namespace bridge;
    const auto normal=e.velocity;
    if (std::abs(length(normal)-1.0f)>0.05f) return;
    const auto position=vec(actor.GetLocation());
    const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    float support=93.15f;
    if (!ball) {
      auto car=gameWrapper->GetLocalCar();
      const auto state=body(car);const auto extent=vec(car.GetLocalCollisionExtent());
      const auto offset=rotate(state.rotation,vec(car.GetLocalCollisionOffset()));
      support=std::abs(dot(normal,rotate(state.rotation,{extent.x,0,0})))+
        std::abs(dot(normal,rotate(state.rotation,{0,extent.y,0})))+
        std::abs(dot(normal,rotate(state.rotation,{0,0,extent.z})))-dot(normal,offset);
      // Preserve wheel clearance above Skyrim surfaces even when the original
      // RL arena no longer reports grounded. Otherwise support flips each tick.
      if (normal.z>0.7f) support=std::max(support,17.0f);
    }
    const auto penetration=support-dot(position-e.position,normal);
    if (penetration < -2.0f || penetration>100.0f) return;
    if (penetration>0) actor.SetLocation(vec(position+normal*(penetration+0.1f)));
    const auto velocity=vec(actor.GetVelocity());const float approach=dot(velocity,normal);
    if (approach<0) actor.AddVelocity(vec(normal*(-approach*(ball?1.6f:1.0f))));
    // This body-plane correction does not supply native suspension traction.
    if (bridge::now_us()-last_contact_log_>1000000) {
      last_contact_log_=bridge::now_us();cvarManager->log("RocketSkyrim Skyrim surface contact "+std::string(ball?"ball":"car")+" penetration="+std::to_string(penetration));
    }
  }
  void events(CarWrapper car) {
    char buffer[1024];std::uint16_t source{};
    for (int i=0;i<32;++i) {
      const int n=socket_.receive(buffer,sizeof buffer,source);
      if (n<0) break;
      bridge::EventPacket e;
      if (source!=cvarManager->getCvar("sb_state_port").getIntValue() ||
          !bridge::decode(buffer,n,e) || e.header.session!=session_ ||
          (have_event_ && !bridge::newer_event(e.header,last_event_time_))) continue;
      // Feedback must refer to a state from the last ~2 seconds of driving.
      const auto age=sequence_-e.target_sequence;
      if (age>(e.type==bridge::EventType::PauseDriving?216000u:((static_cast<unsigned>(e.type)>=4)?12u:240u))) continue;
      have_event_=true;last_event_time_=e.header.timestamp_us;
      last_feedback_=bridge::now_us();
      const auto type=static_cast<unsigned>(e.type);
      if(type>=7 && type<=14) {const int index=(type-7)%4;if(type<11 && std::abs(bridge::length(e.velocity)-1)>0.05f)continue;wheel_hit_[index]=e;wheel_time_[index]=bridge::now_us();}
      else if (e.type==bridge::EventType::CarSurface) surface(car,e,false);
      else if (e.type==bridge::EventType::BallSurface) {
        auto server=gameWrapper->GetCurrentGameState();
        if (!server.IsNull()) { auto ball=server.GetBall();if (!ball.IsNull()) surface(ball,e,true); }
      }
      else if(e.type==bridge::EventType::PauseDriving) {
        if(e.velocity.x>0 || bridge::now_us()>=interaction_hold_)pause_driving(car,e.velocity.x>0);
      }
      else if(e.type==bridge::EventType::ActorContactCar || e.type==bridge::EventType::ActorContactBall) {
        RBActorWrapper actor=car;
        if(e.type==bridge::EventType::ActorContactBall){auto server=gameWrapper->GetCurrentGameState();if(server.IsNull())continue;actor=server.GetBall();}
        if(actor.IsNull())continue;
        auto state=actor.GetCurrentRBState();
        const auto contact=e.position+physics_offset_;
        // Reject distant contact corrections after a respawn or teleport.
        if(bridge::length(vec(state.Location)-contact)>500)continue;
        state.Location=vec(contact);state.LinearVelocity=state.LinearVelocity+vec(e.velocity);
        actor.SetPhysicsState(state);actor.SetLocation(state.Location);actor.SetVelocity(state.LinearVelocity);
        cvarManager->log("RocketSkyrim actor contact recoil="+std::to_string(bridge::length(e.velocity)));
      }
      else if(e.type==bridge::EventType::BumpBall){auto server=gameWrapper->GetCurrentGameState();if(!server.IsNull()){auto ball=server.GetBall();if(!ball.IsNull())ball.AddVelocity(vec(e.velocity));}}
      else if (e.type==bridge::EventType::BumpCar) car.AddVelocity(vec(e.velocity));
      else if (e.type==bridge::EventType::TeleportCar) {
        car.SetLocation(vec(e.position+physics_offset_));car.SetVelocity(vec(e.velocity));
        car.SetRotation(QuatToRotator(sdk_quat(e.rotation)));car.SetAngularVelocity({0,0,0},false);
      } else if (e.type==bridge::EventType::SetBall) {
        auto server=gameWrapper->GetCurrentGameState();
        if (!server.IsNull()) {
          auto ball=server.GetBall();
          if (!ball.IsNull()) {
            ball.SetLocation(vec(e.position+physics_offset_));ball.SetVelocity(vec(e.velocity));
            ball.SetRotation(QuatToRotator(sdk_quat(e.rotation)));ball.SetAngularVelocity({0,0,0},false);
          }
        }
      }
    }
  }
  void tick(CarWrapper caller) {
    // Do not read or alter online-match state, even if the plugin was loaded earlier.
    if (!cvarManager->getCvar("sb_enabled").getBoolValue() || gameWrapper->IsInOnlineGame() ||
        !(gameWrapper->IsInGame() || gameWrapper->IsInFreeplay())) {
      native_terrain_.update(gameWrapper->GetDataFolder()/"rocket-skyrim-terrain.rltm",session_,{},false);if(!gameWrapper->IsInOnlineGame()){auto car=gameWrapper->GetLocalCar();if(!car.IsNull()){physics_origin(car);noclip(car);}}return;
    }
    auto car=gameWrapper->GetLocalCar();
    if (car.IsNull() || car.memory_address!=caller.memory_address) return;
    const auto body_id=car.GetLoadoutBody();
    if (body_id!=logged_body_) {
      logged_body_=body_id;auto items=gameWrapper->GetItemsWrapper();auto product=items.GetProduct(body_id);
      if (!product.IsNull()) cvarManager->log("RocketSkyrim body id="+std::to_string(body_id)+" label="+product.GetLabel().ToString()+" package="+product.GetAssetPackageName()+" path="+product.GetAssetPath().ToString());
    }
    if (sequence_==0) {
      auto ext=car.GetLocalCollisionExtent();auto off=car.GetLocalCollisionOffset();
      cvarManager->log("RocketSkyrim hitbox extent="+std::to_string(ext.X)+","+std::to_string(ext.Y)+","+std::to_string(ext.Z)+" offset="+std::to_string(off.X)+","+std::to_string(off.Y)+","+std::to_string(off.Z));
    }
    native_terrain_.initialize([this](std::string message){cvarManager->log(message);});
    physics_origin(car);
    events(car);
    native_terrain_.update(gameWrapper->GetDataFolder()/"rocket-skyrim-terrain.rltm",session_,vec(car.GetLocation()),
      cvarManager->getCvar("sb_native_terrain").getBoolValue() && (driving_paused_ || bridge::now_us()-last_feedback_<3000000),
      [&](){auto server=gameWrapper->GetCurrentGameState();if(server.IsNull())return bridge::Vec3{};auto ball=server.GetBall();return ball.IsNull()?bridge::Vec3{}:vec(ball.GetLocation());}(),!driving_paused_);
    noclip(car);
    background_controller(car);
    bridge::StatePacket p{};
    p.header={bridge::kMagic,bridge::kVersion,bridge::Kind::State,sizeof p,++sequence_,session_,bridge::now_us()};
    p.flags=bridge::CarValid;p.car=body(car);p.car.position=p.car.position-physics_offset_;
    if (car.IsOnGround()) p.flags|=bridge::OnGround;
    if (car.GetbSuperSonic()) p.flags|=bridge::Supersonic;
    auto boost=car.GetBoostComponent();
    if (!boost.IsNull()) {p.boost=std::clamp(boost.GetCurrentBoostAmount()*100.0f,0.0f,100.0f);if(boost.GetbActive())p.flags|=bridge::Boosting;}
    auto server=gameWrapper->GetCurrentGameState();
    if (!server.IsNull()) { auto ball=server.GetBall();if (!ball.IsNull()) { p.flags|=bridge::BallValid;p.ball=body(ball);p.ball.position=p.ball.position-physics_offset_; } }
    auto camera=gameWrapper->GetCamera();
    if (!camera.IsNull()) {
      const auto pov=camera.GetPOV();
      p.flags|=bridge::CameraValid;p.camera={vec(pov.location)-physics_offset_,bridge_quat(RotatorToQuat(pov.rotation)),pov.FOV};
      if (camera.GetbDemolished()) p.flags|=bridge::Demolished;
    }
    if (!bridge::valid(p)) {
      if (bridge::now_us()-last_log_>1000000) {
        last_log_=bridge::now_us();
        cvarManager->log("RocketSkyrim rejected state flags="+std::to_string(p.flags)+" car="+std::to_string(bridge::valid(p.car))+" ball="+std::to_string(bridge::valid(p.ball))+" FOV="+std::to_string(p.camera.fov)+" boost="+std::to_string(p.boost)+" native="+std::to_string(native_terrain_.applied())+" grounded="+std::to_string(car.IsOnGround())+" wheel_world="+std::to_string(car.GetNumWheelWorldContacts())+" speed="+std::to_string(bridge::length(p.car.velocity)));
      }
      return;
    }
    socket_.send(&p,sizeof p,static_cast<std::uint16_t>(cvarManager->getCvar("sb_state_port").getIntValue()));
    if(bridge::now_us()-last_feedback_<3000000){bridge::EventPacket e{};e.header=p.header;e.header.kind=bridge::Kind::Event;e.header.bytes=sizeof e;e.type=bridge::EventType::CarMenuInput;e.target_sequence=p.header.sequence;e.position=menu_pad_connected_?menu_pad_:bridge::Vec3{};e.velocity.x=menu_pad_connected_?0.f:-1.f;
      socket_.send(&e,sizeof e,static_cast<std::uint16_t>(cvarManager->getCvar("sb_state_port").getIntValue()));}
    if(interaction_pending_){bridge::EventPacket e{};e.header=p.header;e.header.kind=bridge::Kind::Event;e.header.bytes=sizeof e;e.type=bridge::EventType::Interact;e.target_sequence=p.header.sequence;
      socket_.send(&e,sizeof e,static_cast<std::uint16_t>(cvarManager->getCvar("sb_state_port").getIntValue()));interaction_pending_=false;}
    wheels(car,p);
    if (recording_) {
      const std::uint64_t elapsed=p.header.timestamp_us-record_start_;
      const std::uint32_t size=sizeof p;
      recording_.write(reinterpret_cast<const char*>(&elapsed),sizeof elapsed);
      recording_.write(reinterpret_cast<const char*>(&size),sizeof size);
      recording_.write(reinterpret_cast<const char*>(&p),sizeof p);
    }
    if (cvarManager->getCvar("sb_log").getBoolValue() && p.header.timestamp_us-last_log_>1000000) {
      last_log_=p.header.timestamp_us;
      cvarManager->log("RocketSkyrim seq="+std::to_string(p.header.sequence)+" car="+
        std::to_string(p.car.position.x)+","+std::to_string(p.car.position.y)+","+std::to_string(p.car.position.z)+
        " boost="+std::to_string(p.boost)+" native="+std::to_string(native_terrain_.applied())+" grounded="+std::to_string(car.IsOnGround())+" wheel_world="+std::to_string(car.GetNumWheelWorldContacts())+" speed="+std::to_string(bridge::length(p.car.velocity)));
    }
  }
public:
  void onLoad() override {
    try {const auto path=gameWrapper->GetDataFolder()/"rocket-skyrim-resume.txt";
      if(std::filesystem::file_time_type::clock::now()-std::filesystem::last_write_time(path)<std::chrono::seconds(10)){
        std::ifstream f(path);f>>resume_position_.x>>resume_position_.y>>resume_position_.z;resume_pending_=bool(f)&&bridge::valid(resume_position_);
        if(resume_pending_)f>>resume_session_>>resume_sequence_;}
      std::filesystem::remove(path);}catch(const std::filesystem::filesystem_error&){}

    for(const auto* name:{L"xinput1_3.dll",L"xinput1_4.dll",L"xinput9_1_0.dll"}){pad_module_=LoadLibraryW(name);if(pad_module_){poll_pad_=reinterpret_cast<PollPad>(GetProcAddress(pad_module_,"XInputGetState"));if(poll_pad_)break;FreeLibrary(pad_module_);pad_module_=nullptr;}}
    session_=bridge::now_us()^((static_cast<std::uint64_t>(std::random_device{}())<<32)|std::random_device{}());
    if (!session_) session_=1;
    // Preserve a fresh local export across a plugin hot reload so the existing
    // Skyrim calibration can resume without waiting for the next cell export.
    {std::ifstream terrain(gameWrapper->GetDataFolder()/"rocket-skyrim-terrain.rltm",std::ios::binary);bridge::TerrainHeader h{};
      terrain.read(reinterpret_cast<char*>(&h),sizeof h);const auto now=bridge::now_us();
      if(terrain && bridge::valid(h) && now>=h.generation && now-h.generation<5000000){session_=h.session;sequence_=static_cast<std::uint32_t>(now/1000);}}

    // A deliberate hot reload preserves the active stream even when Skyrim's
    // last terrain export is older than the initial-start freshness window.
    if(resume_pending_ && resume_session_){session_=resume_session_;sequence_=resume_sequence_;}
    socket_.open(bridge::kEventPort);
    cvarManager->registerCvar("sb_enabled","1","Send offline RL state to Skyrim",true,true,0,true,1);
    cvarManager->registerCvar("sb_log","1","Log car state once per second",true,true,0,true,1);
    cvarManager->registerCvar("sb_noclip","1","Disable offline arena geometry while Skyrim bridge is active",true,true,0,true,1);
    cvarManager->registerCvar("sb_native_terrain","1","Use real Skyrim triangles in native offline RL physics",true,true,0,true,1);
    cvarManager->registerCvar("sb_wheel_contacts","0","Experimental Skyrim wheel contact injection",true,true,0,true,1);
    cvarManager->registerNotifier("sb_debug_contacts",[this](std::vector<std::string>){
      if(gameWrapper->IsInOnlineGame())return;auto car=gameWrapper->GetLocalCar();if(car.IsNull())return;
      auto sim=car.GetVehicleSim();if(sim.IsNull())return;auto wheels=sim.GetWheels();
      cvarManager->log("RocketSkyrim diagnostics world="+std::to_string(car.GetbCollideWorld())+" grounded="+std::to_string(car.IsOnGround())+" wheelcount="+std::to_string(wheels.Count()));
      for(int i=0;i<std::min(wheels.Count(),4);++i) {auto w=wheels.Get(i);auto c=w.GetContact();auto start=w.GetLocalSuspensionRayStart();cvarManager->log("RocketSkyrim wheel "+std::to_string(i)+" contact="+std::to_string(c.bHasContact)+" material="+std::to_string(c.PhysMatProp!=nullptr)+" template="+std::to_string(wheel_template_[i].PhysMatProp!=nullptr)+" start="+std::to_string(start.X)+","+std::to_string(start.Y)+","+std::to_string(start.Z)+" travel="+std::to_string(w.GetSuspensionTravel())+" raise="+std::to_string(w.GetSuspensionMaxRaise())+" radius="+std::to_string(w.GetWheelRadius())+" response="+std::to_string(wheel_time_[i]!=0));}
    },"Log offline wheel contact diagnostics",PERMISSION_ALL);
    cvarManager->registerCvar("sb_state_port","29741","Skyrim state UDP port",true,true,1024,true,65535);
    cvarManager->registerNotifier("sb_interact",[this](std::vector<std::string>){if(!gameWrapper->IsInOnlineGame() && !driving_paused_){interaction_pending_=true;interaction_hold_=0;}},"Talk/use the nearby Skyrim target while driving",PERMISSION_ALL);
    cvarManager->registerNotifier("sb_start",[this](std::vector<std::string>){
      if (gameWrapper->IsInOnlineGame()) { cvarManager->log("RocketSkyrim requires offline play.");return; }
      cvarManager->getCvar("sb_enabled").setValue(1);
    },"Enable offline bridge",PERMISSION_ALL);
    cvarManager->registerNotifier("sb_stop",[this](std::vector<std::string>){
      if(!gameWrapper->IsInOnlineGame())pause_driving(gameWrapper->GetLocalCar(),false);
      cvarManager->getCvar("sb_enabled").setValue(0);
      native_terrain_.update(gameWrapper->GetDataFolder()/"rocket-skyrim-terrain.rltm",session_,{},false);
      last_feedback_=0;if(!gameWrapper->IsInOnlineGame()) {auto car=gameWrapper->GetLocalCar();if(!car.IsNull()){physics_origin(car);noclip(car);}}
    },"Stop bridge",PERMISSION_ALL);
    cvarManager->registerNotifier("sb_freeplay",[this](std::vector<std::string>){
      if (gameWrapper->IsInOnlineGame()) { cvarManager->log("RocketSkyrim requires offline play.");return; }
      auto training=gameWrapper->GetGfxTrainingData();
      if (training.IsNull()) { cvarManager->log("Freeplay is not ready yet; retry sb_freeplay after startup.");return; }
      cvarManager->getCvar("sb_enabled").setValue(1);
      training.PlayFreeplayMap("EuroStadium_P");
    },"Start an offline freeplay session for bridge validation",PERMISSION_ALL);
    cvarManager->registerNotifier("sb_record",[this](std::vector<std::string> args){
      recording_.close();recording_.clear();
      if (args.size()<2 || args[1]=="stop") return;
      // A basename in the BakkesMod data folder; never write arbitrary paths.
      const auto name=args[1];
      if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) return;
      recording_.open(gameWrapper->GetDataFolder()/(name+".rlsb"),std::ios::binary|std::ios::trunc);
      if (!recording_) { cvarManager->log("Could not open recording");return; }
      recording_.write("RLSBREC1",8);record_start_=bridge::now_us();
      cvarManager->log("Recording "+name+".rlsb");
    },"sb_record <basename|stop>",PERMISSION_ALL);
    gameWrapper->HookEventPost("Function TAGame.Camera_TA.UpdateSwivel",[this](std::string){
      auto camera=gameWrapper->GetCamera();
      if(!background_input_ || driving_paused_ || gameWrapper->IsInOnlineGame() || camera.IsNull())return;
      auto pc=gameWrapper->GetPlayerController();if(pc.IsNull())return;
      auto swivel=camera.GetCurrentSwivel();const auto desired=camera.GetDesiredSwivel(pc.GetALookUp(),background_look_right_);
      swivel.Yaw=desired.Yaw;camera.SetCurrentSwivel(swivel);
      if(std::abs(background_look_right_)>.1f && bridge::now_us()-swivel_log_>1000000){swivel_log_=bridge::now_us();
        cvarManager->log("RocketSkyrim camera swivel horizontal="+std::to_string(background_look_right_)+" yaw="+std::to_string(desired.Yaw));}
    });
    gameWrapper->HookEventWithCallerPost<CarWrapper>(kTick,[this](CarWrapper car,void*,std::string){tick(car);});
    gameWrapper->HookEvent("Function TAGame.GameEvent_Tutorial_TA.Destroyed",[this](std::string){native_terrain_.shutdown();last_feedback_=0;physics_offset_={};offset_car_=offset_ball_=collision_car_=collision_ball_=0;have_previous_physical_=have_ground_position_=driving_paused_=false;});
    native_terrain_.initialize([this](std::string message){cvarManager->log(message);});
    cvarManager->getCvar("sb_enabled").setValue(1);
    cvarManager->log("RocketSkyrim loaded. Offline freeplay: sb_start; sb_record session; sb_stop.");
  }
  void onUnload() override {
    if(!gameWrapper->IsInOnlineGame() && native_terrain_.applied()){auto car=gameWrapper->GetLocalCar();if(!car.IsNull()){
      const auto wire=vec(car.GetLocation())-physics_offset_;std::ofstream f(gameWrapper->GetDataFolder()/"rocket-skyrim-resume.txt");f<<wire.x<<' '<<wire.y<<' '<<wire.z<<' '<<session_<<' '<<sequence_;}}

    if(driving_paused_ && !gameWrapper->IsInOnlineGame()){auto car=gameWrapper->GetLocalCar();if(!car.IsNull())car.SetFrozen(false);
      auto server=gameWrapper->GetCurrentGameState();if(!server.IsNull()){auto ball=server.GetBall();if(!ball.IsNull())ball.SetFrozen(false);}}
    native_terrain_.shutdown();
    last_feedback_=0;if(!gameWrapper->IsInOnlineGame()) {auto car=gameWrapper->GetLocalCar();if(!car.IsNull()){physics_origin(car);noclip(car);}}
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_Tutorial_TA.Destroyed");
    gameWrapper->UnhookEventPost("Function TAGame.Camera_TA.UpdateSwivel");
    gameWrapper->UnhookEventPost(kTick);recording_.close();socket_.close();
    if(pad_module_)FreeLibrary(pad_module_);
  }
};
BAKKESMOD_PLUGIN(RocketSkyrim,"Rocket League to Skyrim offline bridge","0.1.0",PLUGINTYPE_FREEPLAY)
