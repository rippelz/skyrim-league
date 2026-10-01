#pragma once
#include "udp.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace bridge {
// Owns no game objects: all engine work stays on the consumer/game thread.
class PacketReceiver {
public:
  struct Packet { StatePacket state{}; EventPacket event{}; std::uint64_t receipt{}; bool is_state{}; };
  struct Batch { std::deque<Packet> packets; std::uint64_t dropped{}, max_gap_us{}; };
private:
  Udp& socket_;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
  std::mutex mutex_;
  Batch pending_;
  void run() {
    std::uint64_t previous{};
    while (!stopping_.load()) {
      if (!socket_.wait_readable(5)) continue;
      char bytes[1024];std::uint16_t source{};
      for (unsigned i=0;i<256 && !stopping_.load();++i) {
        const auto size=socket_.receive(bytes,sizeof bytes,source);
        if(size<0) break;
        Packet packet;packet.receipt=now_us();
        if(decode(bytes,size,packet.state)) packet.is_state=true;
        else if(!decode(bytes,size,packet.event)) continue;
        std::lock_guard lock(mutex_);
        if(packet.is_state) {
          if(previous) pending_.max_gap_us=std::max(pending_.max_gap_us,packet.receipt-previous);
          previous=packet.receipt;
        }
        // Preserve event ordering with states, bounded even during paused menus.
        if(pending_.packets.size()>=512) {pending_.packets.pop_front();++pending_.dropped;}
        pending_.packets.push_back(packet);
      }
    }
  }
public:
  explicit PacketReceiver(Udp& socket):socket_(socket) {}
  ~PacketReceiver() {stop();}
  void start() {if(worker_.joinable())return;stopping_=false;worker_=std::thread([this]{run();});}
  void stop() {stopping_=true;if(worker_.joinable())worker_.join();}
  Batch drain() {std::lock_guard lock(mutex_);Batch result;std::swap(result,pending_);return result;}
};
}
