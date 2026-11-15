#include "esp_platform.h"

#include <time.h>

#include "certs.h"

namespace {

// Anything earlier means SNTP has not answered yet (2023-11-14).
constexpr time_t kMinValidUnix = 1700000000;

}  // namespace

uint32_t EspTime::millis() { return ::millis(); }

int64_t EspTime::unix_time() {
  const time_t now = time(nullptr);
  return now < kMinValidUnix ? 0 : static_cast<int64_t>(now);
}

bool EspTime::to_local(int64_t unix_time, core::LocalTime& out) {
  const time_t t = static_cast<time_t>(unix_time);
  struct tm local {};
  if (localtime_r(&t, &local) == nullptr) {
    return false;
  }
  out.year = local.tm_year + 1900;
  out.month = static_cast<uint8_t>(local.tm_mon + 1);
  out.day = static_cast<uint8_t>(local.tm_mday);
  out.hour = static_cast<uint8_t>(local.tm_hour);
  out.minute = static_cast<uint8_t>(local.tm_min);
  out.second = static_cast<uint8_t>(local.tm_sec);
  out.weekday = static_cast<uint8_t>(local.tm_wday);
  return true;
}

void EspTransport::begin() {
  anchors_.append(kGtsRootR4);
  anchors_.append(kIsrgRootX2);
  tls_.setTrustAnchors(&anchors_);
  // The servers do not negotiate smaller records (no MFLN), so receiving needs the full
  // 16 KB + 325 B buffer. Requests are small, so the send buffer can shrink.
  tls_.setBufferSizes(16384 + 325, 512);
  tls_.setTimeout(kIdleTimeoutMs);
  plain_.setTimeout(kIdleTimeoutMs);
}

BearSSL::Session* EspTransport::session_for(const etl::istring& host) {
  for (size_t i = 0; i < kSessions; ++i) {
    if (session_hosts_[i] == host) {
      return &sessions_[i];
    }
  }
  // A slot reused for another host just fails resumption once and does a full handshake.
  const size_t slot = next_session_;
  next_session_ = (next_session_ + 1) % kSessions;
  session_hosts_[slot].assign(host.begin(), host.end());
  return &sessions_[slot];
}

bool EspTransport::connect(const core::Url& url) {
  close();
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (url.tls) {
    tls_.setSession(session_for(url.host));
    active_ = &tls_;
  } else {
    active_ = &plain_;
  }
  if (!active_->connect(url.host.c_str(), url.port)) {
    active_->stop();
    active_ = nullptr;
    return false;
  }
  return true;
}

bool EspTransport::write(const char* data, size_t length) {
  if (active_ == nullptr) {
    return false;
  }
  return active_->write(reinterpret_cast<const uint8_t*>(data), length) == length;
}

int EspTransport::read() {
  if (position_ < buffered_) {
    return buffer_[position_++];
  }
  if (active_ == nullptr) {
    return -1;
  }
  const uint32_t start = ::millis();
  for (;;) {
    const int available = active_->available();
    if (available > 0) {
      const size_t want = static_cast<size_t>(available) < sizeof(buffer_)
                              ? static_cast<size_t>(available)
                              : sizeof(buffer_);
      const int got = active_->read(buffer_, want);
      if (got > 0) {
        buffered_ = static_cast<size_t>(got);
        position_ = 1;
        return buffer_[0];
      }
    } else if (!active_->connected()) {
      return -1;
    }
    if (::millis() - start >= kIdleTimeoutMs) {
      return -1;
    }
    delay(1);  // lets Wi-Fi run and feeds the watchdog
  }
}

void EspTransport::close() {
  if (active_ != nullptr) {
    active_->stop();
    active_ = nullptr;
  }
  buffered_ = 0;
  position_ = 0;
}
