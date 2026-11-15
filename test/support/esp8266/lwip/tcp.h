#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using err_t = int8_t;
inline constexpr err_t ERR_OK = 0, ERR_MEM = -1, ERR_INPROGRESS = -5, ERR_RST = -14;
inline constexpr uint8_t TCP_WRITE_FLAG_COPY = 1;
struct ip_addr_t { uint32_t addr = 0; };
inline int ipaddr_aton(const char* text, ip_addr_t* out) {
  if (std::strcmp(text, "192.0.2.1") != 0) return 0;
  out->addr = 1;
  return 1;
}
struct pbuf {
  pbuf* next = nullptr;
  uint16_t len = 0;
  uint16_t tot_len = 0;
  std::vector<uint8_t> bytes;
};
struct tcp_pcb;
using tcp_recv_fn = err_t (*)(void*, tcp_pcb*, pbuf*, err_t);
using tcp_connected_fn = err_t (*)(void*, tcp_pcb*, err_t);
using tcp_err_fn = void (*)(void*, err_t);
using tcp_sent_fn = err_t (*)(void*, tcp_pcb*, uint16_t);
using tcp_poll_fn = err_t (*)(void*, tcp_pcb*);
struct tcp_pcb {
  void* argument = nullptr;
  tcp_recv_fn receive = nullptr;
  tcp_connected_fn connect = nullptr;
  tcp_err_fn error = nullptr;
  bool valid = true;
};
namespace fake_lwip {
inline std::vector<tcp_pcb*> pcbs;
inline size_t freed_pbufs = 0, credited = 0, aborted = 0, closed = 0;
inline uint16_t send_room = 256;
inline err_t write_result = ERR_OK, connect_result = ERR_OK, close_result = ERR_OK;
inline bool allocation_fails = false;
inline std::vector<uint8_t> sent;
inline void reset() {
  for (auto* pcb : pcbs) delete pcb;
  pcbs.clear();
  freed_pbufs = credited = aborted = closed = 0;
  send_room = 256;
  write_result = connect_result = close_result = ERR_OK;
  allocation_fails = false;
  sent.clear();
}
inline tcp_pcb* latest() { assert(!pcbs.empty()); return pcbs.back(); }
inline void check(tcp_pcb* pcb) { assert(pcb != nullptr && pcb->valid); }
inline void establish() { auto* pcb = latest(); check(pcb); pcb->connect(pcb->argument, pcb, ERR_OK); }
inline void reset_peer() {
  auto* pcb = latest(); check(pcb);
  auto callback = pcb->error;
  auto* argument = pcb->argument;
  pcb->valid = false;
  callback(argument, ERR_RST);
}
inline err_t deliver(pbuf* data) {
  auto* pcb = latest(); check(pcb);
  return pcb->receive(pcb->argument, pcb, data, ERR_OK);
}
inline pbuf* packet(std::initializer_list<uint8_t> bytes, pbuf* tail = nullptr) {
  auto* result = new pbuf;
  result->bytes = bytes;
  result->next = tail;
  result->len = static_cast<uint16_t>(bytes.size());
  result->tot_len = result->len + (tail == nullptr ? 0 : tail->tot_len);
  return result;
}
}
inline tcp_pcb* tcp_new() {
  if (fake_lwip::allocation_fails) return nullptr;
  auto* pcb = new tcp_pcb;
  fake_lwip::pcbs.push_back(pcb);
  return pcb;
}
inline void tcp_arg(tcp_pcb* pcb, void* arg) { fake_lwip::check(pcb); pcb->argument = arg; }
inline void tcp_recv(tcp_pcb* pcb, tcp_recv_fn fn) { fake_lwip::check(pcb); pcb->receive = fn; }
inline void tcp_sent(tcp_pcb* pcb, tcp_sent_fn) { fake_lwip::check(pcb); }
inline void tcp_err(tcp_pcb* pcb, tcp_err_fn fn) { fake_lwip::check(pcb); pcb->error = fn; }
inline void tcp_poll(tcp_pcb* pcb, tcp_poll_fn, uint8_t) { fake_lwip::check(pcb); }
inline err_t tcp_connect(tcp_pcb* pcb, const ip_addr_t*, uint16_t, tcp_connected_fn fn) {
  fake_lwip::check(pcb); pcb->connect = fn; return fake_lwip::connect_result;
}
inline void tcp_abort(tcp_pcb* pcb) {
  fake_lwip::check(pcb);
  assert(pcb->argument == nullptr && pcb->receive == nullptr && pcb->error == nullptr);
  pcb->valid = false; ++fake_lwip::aborted;
}
inline err_t tcp_close(tcp_pcb* pcb) {
  fake_lwip::check(pcb);
  assert(pcb->argument == nullptr && pcb->receive == nullptr && pcb->error == nullptr);
  ++fake_lwip::closed;
  if (fake_lwip::close_result == ERR_OK) pcb->valid = false;
  return fake_lwip::close_result;
}
inline uint16_t tcp_sndbuf(tcp_pcb* pcb) { fake_lwip::check(pcb); return fake_lwip::send_room; }
inline err_t tcp_write(tcp_pcb* pcb, const void* data, uint16_t count, uint8_t flags) {
  fake_lwip::check(pcb); assert(flags == TCP_WRITE_FLAG_COPY);
  if (fake_lwip::write_result == ERR_OK) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    fake_lwip::sent.insert(fake_lwip::sent.end(), bytes, bytes + count);
  }
  return fake_lwip::write_result;
}
inline err_t tcp_output(tcp_pcb* pcb) { fake_lwip::check(pcb); return ERR_OK; }
inline void tcp_recved(tcp_pcb* pcb, uint16_t count) { fake_lwip::check(pcb); fake_lwip::credited += count; }
inline uint16_t pbuf_copy_partial(const pbuf* p, void* out, uint16_t count, uint16_t offset) {
  auto* destination = static_cast<uint8_t*>(out);
  uint16_t copied = 0;
  while (p != nullptr && copied < count) {
    if (offset >= p->len) offset -= p->len;
    else {
      const uint16_t n = std::min<uint16_t>(count - copied, p->len - offset);
      std::memcpy(destination + copied, p->bytes.data() + offset, n);
      copied += n;
      offset = 0;
    }
    p = p->next;
  }
  return copied;
}
inline uint8_t pbuf_free(pbuf* p) {
  uint8_t count = 0;
  while (p != nullptr) { auto* next = p->next; delete p; p = next; ++count; ++fake_lwip::freed_pbufs; }
  return count;
}
