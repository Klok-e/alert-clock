#pragma once

#include <cstdint>
#include <string>

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

// Plain TCP sockets with a receive timeout. https:// is refused: the PC build only
// talks to the mock server.
class SocketTransport final : public core::Transport, private core::ByteSource {
 public:
  ~SocketTransport();

  bool connect(const core::Url& url) override;
  bool write(const char* data, size_t length) override;
  core::ByteSource& input() override { return *this; }
  void close() override;
  core::ReadEnd read_end() const override { return read_end_; }

 private:
  int read() override;
  core::ReadEnd read_end_ = core::ReadEnd::None;

  int fd_ = -1;
  char buffer_[4096];
  long buffered_ = 0;
  long position_ = 0;
};

// Nonces from the operating system's random source.
class HostRandom final : public core::Random {
 public:
  uint32_t next() override;
};

// GET `url` (plus "?query" when given) and return the HTTP status, or -1 on transport failure.
int http_get_status(const std::string& url, const char* query = nullptr);
