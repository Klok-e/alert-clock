#pragma once

#include <cstdint>
#include <string>

#include <netdb.h>
#include <sys/socket.h>

#include "app.h"

// Time under the scenario's control: steps set it explicitly.
class FakeTime final : public core::TimeSource {
 public:
  void set(int64_t unix_time, uint32_t millis) {
    unix_time_ = unix_time;
    millis_ = millis;
  }

  uint32_t millis() override { return millis_; }
  int64_t unix_time() override { return unix_time_; }
  bool to_local(int64_t unix_time, core::LocalTime& out) override;

 private:
  int64_t unix_time_ = 0;
  uint32_t millis_ = 0;
};

// Plain nonblocking TCP; HTTPS is refused because native runs use local mocks.
class SocketTransport final : public core::Transport {
 public:
  ~SocketTransport();
  SocketTransport() = default;
  SocketTransport(const SocketTransport&) = delete;
  SocketTransport& operator=(const SocketTransport&) = delete;

  bool begin(const core::Url& url, uint32_t now) override;
  core::LinkState advance(uint32_t now) override;
  core::IoResult write_some(const uint8_t* data, size_t length) override;
  core::IoResult read_some(uint8_t* data, size_t capacity) override;
  void close() override;
  core::ReadEnd read_end() const override { return read_end_; }

 private:
  enum class State : uint8_t { Idle, Resolve, Connect, Ready, Failed };
  core::LinkState next_address();

  core::ReadEnd read_end_ = core::ReadEnd::None;
  State state_ = State::Idle;
  int fd_ = -1;
  etl::string<64> host_;
  uint16_t port_ = 0;
  uint64_t resolver_token_ = 0;
  bool addresses_ready_ = false;
  addrinfo* addresses_ = nullptr;
  addrinfo* next_ = nullptr;
  sockaddr_storage numeric_{};
  socklen_t numeric_length_ = 0;
};

// Nonces from the operating system's random source.
class HostRandom final : public core::Random {
 public:
  uint32_t next() override;
};

// A bounded blocking convenience for scenario setup, outside the App loop.
int http_get_status(const std::string& url, const char* query = nullptr);
