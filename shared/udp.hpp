#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bridge {
inline std::uint64_t now_us() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count());
}
class Udp {
#ifdef _WIN32
  SOCKET socket_{INVALID_SOCKET};
  bool initialized_{};
#else
  int socket_{-1};
#endif
public:
  Udp() = default;
  Udp(const Udp&)=delete; Udp& operator=(const Udp&)=delete;
  ~Udp() { close(); }
  void open(std::uint16_t port) {
    close();
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2),&data)!=0) throw std::runtime_error("WSAStartup failed");
    initialized_=true;
    socket_=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (socket_==INVALID_SOCKET) { close(); throw std::runtime_error("UDP socket failed"); }
    BOOL exclusive=TRUE;
    setsockopt(socket_,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof exclusive);
    u_long nonblocking=1;
    if (ioctlsocket(socket_,FIONBIO,&nonblocking)!=0) { close(); throw std::runtime_error("UDP nonblocking failed"); }
#else
    socket_=::socket(AF_INET,SOCK_DGRAM,0);
    if (socket_<0) throw std::runtime_error("UDP socket failed");
    if (fcntl(socket_,F_SETFL,O_NONBLOCK)<0) { close(); throw std::runtime_error("UDP nonblocking failed"); }
#endif
    int receive_buffer=256*1024;
    setsockopt(socket_,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&receive_buffer),sizeof receive_buffer);
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=htons(port);
    if (::bind(socket_,reinterpret_cast<sockaddr*>(&addr),sizeof addr)!=0) {
      close();throw std::runtime_error("Cannot bind loopback UDP port "+std::to_string(port));
    }
  }
  void close() {
#ifdef _WIN32
    if (socket_!=INVALID_SOCKET) { closesocket(socket_);socket_=INVALID_SOCKET; }
    if (initialized_) { WSACleanup();initialized_=false; }
#else
    if (socket_>=0) { ::close(socket_);socket_=-1; }
#endif
  }
  bool send(const void* data, std::size_t bytes, std::uint16_t port) {
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=htons(port);
    return sendto(socket_,reinterpret_cast<const char*>(data),static_cast<int>(bytes),0,
      reinterpret_cast<sockaddr*>(&addr),sizeof addr)==static_cast<int>(bytes);
  }
  bool wait_readable(unsigned milliseconds) {
    fd_set readable;FD_ZERO(&readable);FD_SET(socket_,&readable);
    timeval timeout{};timeout.tv_sec=milliseconds/1000;timeout.tv_usec=(milliseconds%1000)*1000;
#ifdef _WIN32
    return select(0,&readable,nullptr,nullptr,&timeout)>0;
#else
    return select(socket_+1,&readable,nullptr,nullptr,&timeout)>0;
#endif
  }
  int receive(void* data, std::size_t capacity, std::uint16_t& source_port) {
    sockaddr_in addr{};
#ifdef _WIN32
    int size=sizeof addr;
#else
    socklen_t size=sizeof addr;
#endif
    const auto n=recvfrom(socket_,reinterpret_cast<char*>(data),static_cast<int>(capacity),0,
      reinterpret_cast<sockaddr*>(&addr),&size);
    if (n<0) return -1;
    if (addr.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)) return 0;
    source_port=ntohs(addr.sin_port);return static_cast<int>(n);
  }
};
}
