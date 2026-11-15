#pragma once

#include <ESP8266WiFi.h>

#include <etl/string.h>

#include "app.h"

// Wall-clock time from SNTP (configTime) and the board's millisecond counter.
class EspTime final : public core::TimeSource {
 public:
  uint32_t millis() override;
  int64_t unix_time() override;
  bool to_local(int64_t unix_time, core::LocalTime& out) override;
};

// Plain TCP to the gateway. Reads are buffered and time out after kIdleTimeoutMs without data.
class EspTransport final : public core::Transport, private core::ByteSource {
 public:
  void begin();

  bool connect(const core::Url& url) override;
  bool write(const char* data, size_t length) override;
  core::ByteSource& input() override { return *this; }
  void close() override;
  core::ReadEnd read_end() const override { return read_end_; }

 private:
  static constexpr uint32_t kIdleTimeoutMs = 5000;
  static constexpr uint32_t kConnectTimeoutMs = 2000;  // the gateway is on the LAN

  int read() override;
  core::ReadEnd read_end_ = core::ReadEnd::None;

  WiFiClient client_;
  bool open_ = false;
  uint8_t buffer_[256];
  size_t buffered_ = 0;
  size_t position_ = 0;
};

// The ESP8266's hardware random number generator.
class EspRandom final : public core::Random {
 public:
  uint32_t next() override;
};

// The gateway address ("host" or "host:port"), kept in the emulated EEPROM sector of flash.
namespace gateway_store {
constexpr size_t kMaxAddress = 63;
void begin();
// The saved address, or "" when none is saved.
const char* load();
bool save(const char* address);
}  // namespace gateway_store
