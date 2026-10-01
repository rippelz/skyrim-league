#include "shared/timeline.hpp"
#include "shared/packet_receiver.hpp"
#include <cstdlib>
#include <iostream>
using namespace bridge;
void check(bool ok,const char* message) {if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
StatePacket state(unsigned sequence,std::uint64_t stamp) {
 StatePacket p{};p.header={kMagic,kVersion,Kind::State,sizeof p,sequence,42,stamp};
 p.flags=CarValid|BallValid|CameraValid;
 p.car.position={float(stamp-100000)/1000,0,17};p.car.velocity={1000,0,0};
 p.ball.position=p.car.position+Vec3{100,0,0};p.ball.velocity={2000,0,0};
 p.camera.position=p.car.position+Vec3{-300,0,100};return p;
}
int main() {
 Timeline timeline;
 check(timeline.push(state(1,100000),500000),"first packet rejected");
 check(timeline.push(state(2,110000),510000),"second packet rejected");
 // A late packet must not reset the playback target by its arrival jitter.
 check(timeline.push(state(3,120000),536000),"delayed packet rejected");
 auto p=timeline.sample(536000,30000);
 check(p && std::abs(p->car.position.x-6)<.001f,"late arrival moved playback clock");
 auto before=timeline.sample(539999,30000);
 check(timeline.push(state(4,130000),540000),"next packet rejected");
 auto after=timeline.sample(540000,30000);
 check(before && after && std::abs(after->car.position.x-before->car.position.x)<.01f,"new packet made presentation jump");
 p=timeline.sample(555000,10000);
 check(p && std::abs(p->car.position.x-45)<.001f,"short gap was not predicted");
 check(length((p->camera.position-p->car.position)-Vec3{-300,0,100})<.001f,"camera prediction desynced car");
 check(std::abs(p->ball.position.x-160)<.001f,"ball prediction wrong");
 p=timeline.sample(900000,10000);
 check(p && std::abs(p->car.position.x-50)<.001f,"prediction exceeded 20 ms bound");
 check(std::abs(timeline.latest()->car.position.x-30)<.001f,"prediction modified authoritative state");
 check(!timeline.sample(1040001,10000),"dead stream remained fresh");
 auto reset=state(5,140000);reset.car.position={4000,0,17};reset.ball.position={6000,0,17};
 check(timeline.push(reset,540000),"batched receipt rejected");
 p=timeline.sample(580000,10000);
 check(p && p->car.position.x==4000 && p->ball.position.x==6000,"teleport was extrapolated");
 Timeline ball_reset;
 ball_reset.push(state(1,100000),500000);auto ball=state(2,110000);ball.ball.position.x=6000;
 ball_reset.push(ball,510000);p=ball_reset.sample(535000,30000);
 check(p && p->ball.position.x==6000,"ball reset blended through world");
 p=ball_reset.sample(540000,10000);
 check(p && p->ball.position.x==6000 && p->car.position.x==30,"ball teleport prediction or car prediction wrong");
 // Background receiver keeps receipt times independent of consumer/frame stalls.
 Udp socket,sender;socket.open(39743);sender.open(0);PacketReceiver receiver(socket);receiver.start();
 auto a=state(1,100000),b=state(2,120000);
 check(sender.send(&a,sizeof a,39743),"first loopback send failed");
 std::this_thread::sleep_for(std::chrono::milliseconds(25));
 check(sender.send(&b,sizeof b,39743),"second loopback send failed");
 std::this_thread::sleep_for(std::chrono::milliseconds(40));
 auto batch=receiver.drain();
 check(batch.packets.size()==2,"background receiver lost packets during frame stall");
 check(batch.packets.back().receipt-batch.packets.front().receipt>=10000,"receipt timestamps collapsed to consumer frame time");
 check(batch.max_gap_us>=10000 && batch.dropped==0,"receiver timing metrics wrong");
 check(now_us()-batch.packets.back().receipt>=20000,"receipt used consumer time");
 receiver.stop();
 std::cout<<"Stable playback clock, bounded prediction, camera coherence, resets and background UDP reception passed\n";
}
