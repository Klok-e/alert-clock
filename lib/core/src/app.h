#pragma once

#include <cstddef>
#include <cstdint>

#include <etl/string.h>

#include "alerts.h"
#include "button.h"
#include "http.h"
#include "screen.h"
#include "threats.h"
#include "view.h"

namespace core {

// A byte pipe to one server. The clock wraps WiFiClient/BearSSL, the PC build wraps sockets.
class Transport {
 public:
  virtual bool connect(const Url& url) = 0;  // TLS when url.tls
  virtual bool write(const char* data, size_t length) = 0;
  virtual ByteSource& input() = 0;           // response bytes; -1 at close or timeout
  virtual void close() = 0;

 protected:
  ~Transport() = default;
};

class TimeSource {
 public:
  virtual uint32_t millis() = 0;    // monotonic; wraps after ~49 days
  virtual int64_t unix_time() = 0;  // 0 until the clock is set
  virtual bool to_local(int64_t unix_time, LocalTime& out) = 0;

 protected:
  ~TimeSource() = default;
};

// Strings are not copied: every pointer must outlive the App (use literals or statics).
struct Settings {
  LatLon home;
  const char* home_region = nullptr;  // oblast name as NEPTUN spells it; optional
  float map_radius_km = 30.0f;        // half the screen width
  float watch_radius_km = 25.0f;      // tracks inside switch the clock to the map
  const char* threats_url = "https://neptun.in.ua/api/v1/threats";
  const char* alerts_url = nullptr;   // alerts.in.ua v2 IoT URL for one region; nullptr disables
  const char* alerts_token = nullptr;
  uint32_t threats_period_ms = 15000;  // raised to 5 s if lower (NEPTUN's limit)
  uint32_t alerts_period_ms = 20000;   // raised to 7.5 s if lower (alerts.in.ua: 8 a minute)
  uint32_t fresh_for_ms = 180000;      // older data counts as missing
  uint32_t map_request_ms = 30000;     // how long a short press keeps the map up
};

enum class Sound : uint8_t { None, AlertRaised, AlertEscalated, AlertCleared };
const char* sound_name(Sound sound);

enum class FetchOutcome : uint8_t {
  None,
  Ok,
  NotModified,
  ConnectFailed,
  RequestFailed,
  BadHead,
  HttpError,
  ParseFailed,
};
const char* fetch_outcome_name(FetchOutcome outcome);

struct SourceStatus {
  FetchOutcome last_outcome = FetchOutcome::None;
  int last_http_status = 0;
  uint32_t successes = 0;
  uint32_t failures = 0;
  uint8_t consecutive_failures = 0;
  bool ever_succeeded = false;
  bool expired = false;  // latched once the data outlives fresh_for_ms; cleared by a success
  uint32_t last_success_ms = 0;
  uint32_t next_due_ms = 0;
};

class App {
 public:
  App(const Settings& settings, TimeSource& time, Transport& transport);

  // Runs at most one network fetch per call; nothing is fetched until the clock is set.
  void tick();
  void press(ButtonEvent event);
  // Brings both fetches forward, but never closer than each API's minimum interval to the
  // previous request, and never into the wait after a 429.
  void refresh_now();
  // Points the sources elsewhere, e.g. at the mock server. False (and no change) if a URL
  // is invalid. nullptr leaves that source unchanged.
  bool set_urls(const char* threats_url, const char* alerts_url);
  void build_view(ViewModel& view);
  // The pending sound, cleared by reading it.
  Sound take_sound();

  const Settings& settings() const { return settings_; }
  const SourceStatus& threats_status() const { return threats_.status; }
  const SourceStatus& alerts_status() const { return alerts_.status; }
  const ThreatSnapshot& threats() const { return snapshot_; }
  AlertLevel alert_level() const { return alert_level_; }
  ThreatParse last_threat_parse() const { return last_parse_; }

 private:
  struct Source {
    Url url;
    bool configured = false;
    bool forced = false;          // a refresh was requested
    bool requested = false;       // a request has been sent at least once
    uint32_t last_request_ms = 0;
    uint32_t min_interval_ms = 0;  // the API's limit between requests
    etl::string<96> etag;
    etl::string<48> last_modified;
    uint64_t success_uptime_ms = 0;  // for the shown age, which must not wrap like millis()
    SourceStatus status;
  };

  // millis() extended to 64 bits; each call must come within ~49.7 days of the previous one.
  uint64_t advance_uptime();

  bool due(const Source& source, uint32_t now) const;
  bool fresh(const Source& source, uint32_t now) const;
  void expire_if_old(Source& source, uint32_t now);
  void record(Source& source, FetchOutcome outcome, uint32_t period_ms);
  FetchOutcome fetch(Source& source, bool threats);
  FetchOutcome read_alert(ByteSource& body);
  FetchOutcome read_threats(ByteSource& body);

  Settings settings_;
  TimeSource& time_;
  Transport& transport_;
  Source threats_;
  Source alerts_;
  RequestBuffer request_;
  ThreatSnapshot snapshot_;
  ThreatSnapshot incoming_;
  ThreatParse last_parse_ = ThreatParse::Ok;
  AlertLevel alert_level_ = AlertLevel::Unknown;
  AlertTracker tracker_;
  Sound pending_sound_ = Sound::None;
  bool map_requested_ = false;
  uint32_t map_requested_at_ = 0;
  uint64_t uptime_ms_ = 0;
  uint32_t uptime_millis_ = 0;
};

}  // namespace core
