#pragma once

#include <ESP8266WiFi.h>

#include <lwip/dns.h>
#include <lwip/tcp.h>

#include "device_store.h"

#include "app.h"

// Wall-clock time from SNTP (configTime) and the board's millisecond counter.
class EspTime final : public core::TimeSource {
 public:
  uint32_t millis() override;
  int64_t unix_time() override;
  bool to_local(int64_t unix_time, core::LocalTime& out) override;
};

// The pbuf chain remains owned by this transport until read_some consumes it.
class EspTransport final : public core::Transport {
 public:
  EspTransport() = default;
  EspTransport(const EspTransport&) = delete;
  EspTransport& operator=(const EspTransport&) = delete;
  ~EspTransport();
  bool begin(const core::Url& url, uint32_t now_ms) override;
  core::LinkState advance(uint32_t now_ms) override;
  core::IoResult write_some(const uint8_t* data, size_t length) override;
  core::IoResult read_some(uint8_t* data, size_t capacity) override;
  void close() override;
  core::ReadEnd read_end() const override { return read_end_; }

 private:
  struct DnsTicket {
    EspTransport* owner = nullptr;
    uint64_t generation = 0;
    bool occupied = false;
  };
  static DnsTicket dns_tickets_[3];
  static void dns_found(const char*, const ip_addr_t*, void*);
  static err_t connected(void*, tcp_pcb*, err_t);
  static err_t received(void*, tcp_pcb*, pbuf*, err_t);
  static void failed(void*, err_t);
  void detach_and_close(bool abort_connection);

  tcp_pcb* pcb_ = nullptr;
  pbuf* received_ = nullptr;
  ip_addr_t address_{};
  uint64_t generation_ = 0;
  size_t offset_ = 0;
  uint16_t port_ = 0;
  core::ReadEnd read_end_ = core::ReadEnd::None;
  core::LinkState state_ = core::LinkState::Failed;
  bool address_ready_ = false;
  bool eof_ = false;
};

// The ESP8266's hardware random number generator.
class EspRandom final : public core::Random {
 public:
  uint32_t next() override;
};
