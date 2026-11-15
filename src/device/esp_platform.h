#pragma once

#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>

#include <etl/string.h>

#include "app.h"

// Wall-clock time from SNTP (configTime) and the board's millisecond counter.
class EspTime final : public core::TimeSource {
 public:
  uint32_t millis() override;
  int64_t unix_time() override;
  bool to_local(int64_t unix_time, core::LocalTime& out) override;
};

// Plain TCP for http:// (the mock server) and BearSSL TLS for https://, with pinned
// root certificates and session resumption. Reads are buffered and time out after
// kIdleTimeoutMs without data.
class EspTransport final : public core::Transport, private core::ByteSource {
 public:
  void begin();  // parses the trust anchors; call once in setup()

  bool connect(const core::Url& url) override;
  bool write(const char* data, size_t length) override;
  core::ByteSource& input() override { return *this; }
  void close() override;

 private:
  static constexpr uint32_t kIdleTimeoutMs = 8000;
  static constexpr size_t kSessions = 2;

  int read() override;
  BearSSL::Session* session_for(const etl::istring& host);

  WiFiClient plain_;
  BearSSL::WiFiClientSecure tls_;
  BearSSL::X509List anchors_;
  BearSSL::Session sessions_[kSessions];
  etl::string<64> session_hosts_[kSessions];
  size_t next_session_ = 0;
  WiFiClient* active_ = nullptr;
  uint8_t buffer_[256];
  size_t buffered_ = 0;
  size_t position_ = 0;
};
