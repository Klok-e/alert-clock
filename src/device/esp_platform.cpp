#include "esp_platform.h"

#include <EEPROM.h>
#include <time.h>

namespace {

// Anything earlier means SNTP has not answered yet (2023-11-14).
constexpr time_t kMinValidUnix = 1700000000;

constexpr uint32_t kStoreMagic = 0x47575931;  // "GWY1"
constexpr size_t kStoreBytes = 4 + gateway_store::kMaxAddress + 1;
char stored_address[gateway_store::kMaxAddress + 1] = "";

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

// WiFiClient::connect waits up to the stream timeout; reads use kIdleTimeoutMs in read().
void EspTransport::begin() { client_.setTimeout(kConnectTimeoutMs); }

bool EspTransport::connect(const core::Url& url) {
  close();
  read_end_ = core::ReadEnd::None;
  if (url.tls || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (!client_.connect(url.host.c_str(), url.port)) {
    client_.stop();
    return false;
  }
  client_.setNoDelay(true);
  open_ = true;
  return true;
}

bool EspTransport::write(const char* data, size_t length) {
  if (!open_) {
    return false;
  }
  return client_.write(reinterpret_cast<const uint8_t*>(data), length) == length;
}

int EspTransport::read() {
  if (position_ < buffered_) {
    return buffer_[position_++];
  }
  if (!open_) {
    read_end_ = core::ReadEnd::Closed;
    return -1;
  }
  const uint32_t start = ::millis();
  for (;;) {
    // Sample closure before reading so bytes delivered with FIN are drained before EOF.
    const bool connected = client_.connected();
    const int got = client_.read(buffer_, sizeof(buffer_));
    if (got > 0) {
      buffered_ = static_cast<size_t>(got);
      position_ = 1;
      return buffer_[0];
    }
    if (!connected) {
      read_end_ = core::ReadEnd::Closed;
      return -1;
    }
    if (::millis() - start >= kIdleTimeoutMs) {
      read_end_ = core::ReadEnd::Timeout;
      return -1;
    }
    delay(1);  // lets Wi-Fi run and feeds the watchdog
  }
}

void EspTransport::close() {
  if (open_) {
    client_.stop();
    open_ = false;
  }
  buffered_ = 0;
  position_ = 0;
}

uint32_t EspRandom::next() { return RANDOM_REG32; }

namespace gateway_store {

void begin() {
  EEPROM.begin(kStoreBytes);
  uint32_t magic = 0;
  EEPROM.get(0, magic);
  if (magic != kStoreMagic) {
    stored_address[0] = '\0';
    return;
  }
  for (size_t i = 0; i <= kMaxAddress; ++i) {
    stored_address[i] = static_cast<char>(EEPROM.read(static_cast<int>(4 + i)));
  }
  stored_address[kMaxAddress] = '\0';
}

const char* load() { return stored_address; }

bool save(const char* address) {
  const size_t length = strnlen(address, kMaxAddress + 1);
  if (length > kMaxAddress) {
    return false;
  }
  EEPROM.put(0, kStoreMagic);
  for (size_t i = 0; i <= kMaxAddress; ++i) {
    const char c = i < length ? address[i] : '\0';
    EEPROM.write(static_cast<int>(4 + i), static_cast<uint8_t>(c));
    stored_address[i] = c;
  }
  return EEPROM.commit();
}

}  // namespace gateway_store
