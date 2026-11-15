#include "service.h"

#include <ESP8266WiFi.h>

namespace {

constexpr const char* kUser = "admin";
constexpr uint32_t kRestartDelayMs = 1500;
// The state document is about 600 bytes; it is formatted on the stack, from a format
// string kept in flash, so it costs no static RAM.
constexpr size_t kStateBytes = 896;

// Every value below is a number, a boolean or one of the fixed enum names, so no
// JSON escaping is needed.
const char kStateFormat[] PROGMEM =
    "{\"uptime_s\":%lu,\"safe_mode\":%s,\"crash_boots\":%lu,\"reset_reason\":%d,"
    "\"heap\":{\"free\":%lu,\"max_block\":%lu,\"fragmentation\":%u},"
    "\"wifi\":{\"connected\":%s,\"rssi\":%d},"
    "\"view\":{\"panel\":\"%s\",\"banner\":\"%s\",\"time_valid\":%s,\"markers\":%u,"
    "\"threats_in_watch\":%u},"
    "\"alert\":{\"level\":\"%s\",\"outcome\":\"%s\",\"http\":%d,\"successes\":%lu,\"failures\":%lu},"
    "\"threats\":{\"outcome\":\"%s\",\"http\":%d,\"successes\":%lu,\"failures\":%lu,"
    "\"parse\":\"%s\",\"nearby\":%u,\"total\":%u,\"rejected\":%u,\"area_only\":%u,"
    "\"overflow\":%s,\"arena_high_water\":%u}}\n";

const char* json_bool(bool value) { return value ? "true" : "false"; }

const char kWifiForm[] PROGMEM =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>alert-clock Wi-Fi</title><h3>alert-clock Wi-Fi</h3>"
    "<form method=post action=/wifi><p>Network<br><input name=ssid maxlength=32 required>"
    "<p>Password<br><input name=password type=password maxlength=64>"
    "<p><button>Save and restart</button></form>";

}  // namespace

void Service::begin(const char* admin_password) {
  password_ = admin_password;

  server_.on("/", HTTP_GET, [this]() {
    server_.send(200, "text/plain",
                 "alert-clock: /wifi /debug/state /debug/press /debug/refresh /debug/sources /update\n");
  });
  server_.on("/wifi", HTTP_GET, [this]() {
    if (authorized()) {
      server_.send_P(200, "text/html", kWifiForm);
    }
  });
  server_.on("/wifi", HTTP_POST, [this]() {
    if (authorized()) {
      save_wifi();
    }
  });
  server_.on("/debug/state", HTTP_GET, [this]() {
    if (authorized()) {
      send_state();
    }
  });
  server_.on("/debug/press", HTTP_POST, [this]() {
    if (!authorized()) {
      return;
    }
    const bool long_press = server_.arg("kind") == "long";
    app_.press(long_press ? core::ButtonEvent::Long : core::ButtonEvent::Short);
    server_.send(200, "text/plain", "ok\n");
  });
  server_.on("/debug/refresh", HTTP_POST, [this]() {
    if (!authorized()) {
      return;
    }
    app_.refresh_now();
    server_.send(200, "text/plain", "ok\n");
  });
  server_.on("/debug/sources", HTTP_POST, [this]() {
    if (!authorized()) {
      return;
    }
    // References to the server's own copies; the short names fit String's inline buffer.
    const String& threats = server_.arg("threats");
    const String& alerts = server_.arg("alerts");
    const bool ok = app_.set_urls(threats.length() > 0 ? threats.c_str() : nullptr,
                                  alerts.length() > 0 ? alerts.c_str() : nullptr);
    server_.send(ok ? 200 : 400, "text/plain", ok ? "ok\n" : "invalid url\n");
  });
  updater_.setup(&server_, "/update", kUser, password_);
  server_.begin();
}

void Service::handle() {
  server_.handleClient();
  if (restart_at_ms_ != 0 && static_cast<int32_t>(millis() - restart_at_ms_) >= 0) {
    ESP.restart();
  }
}

void Service::save_wifi() {
  const String& ssid = server_.arg("ssid");
  const String& password = server_.arg("password");
  const bool valid = ssid.length() >= 1 && ssid.length() <= 32 &&
                     (password.length() == 0 || (password.length() >= 8 && password.length() <= 64));
  if (!valid) {
    server_.send(400, "text/plain", "ssid 1-32 chars; password empty or 8-64 chars\n");
    return;
  }
  WiFi.persistent(true);  // the SDK keeps the credentials in flash
  WiFi.begin(ssid.c_str(), password.c_str());
  server_.send(200, "text/plain", "saved; restarting\n");
  restart_at_ms_ = millis() + kRestartDelayMs;
  if (restart_at_ms_ == 0) {
    restart_at_ms_ = 1;
  }
}

bool Service::authorized() {
  if (server_.authenticate(kUser, password_)) {
    return true;
  }
  server_.requestAuthentication();
  return false;
}

void Service::send_state() {
  const rst_info* reset = ESP.getResetInfoPtr();
  const core::SourceStatus& alerts = app_.alerts_status();
  const core::SourceStatus& threats = app_.threats_status();
  const core::ThreatSnapshot& snapshot = app_.threats();
  char text[kStateBytes];
  const int length = snprintf_P(
      text, sizeof(text), kStateFormat, static_cast<unsigned long>(millis() / 1000),
      json_bool(safe_mode_.active()), static_cast<unsigned long>(safe_mode_.crash_boots()),
      reset != nullptr ? static_cast<int>(reset->reason) : -1,
      static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxFreeBlockSize()),
      static_cast<unsigned>(ESP.getHeapFragmentation()), json_bool(WiFi.status() == WL_CONNECTED),
      static_cast<int>(WiFi.RSSI()), core::panel_name(view_.panel), core::banner_name(view_.banner),
      json_bool(view_.time_valid), static_cast<unsigned>(view_.markers.size()),
      static_cast<unsigned>(view_.threats_in_watch), core::alert_level_name(app_.alert_level()),
      core::fetch_outcome_name(alerts.last_outcome), alerts.last_http_status,
      static_cast<unsigned long>(alerts.successes), static_cast<unsigned long>(alerts.failures),
      core::fetch_outcome_name(threats.last_outcome), threats.last_http_status,
      static_cast<unsigned long>(threats.successes), static_cast<unsigned long>(threats.failures),
      core::threat_parse_name(app_.last_threat_parse()), static_cast<unsigned>(snapshot.nearby.size()),
      static_cast<unsigned>(snapshot.total), static_cast<unsigned>(snapshot.rejected),
      static_cast<unsigned>(snapshot.area_only), json_bool(snapshot.overflow),
      static_cast<unsigned>(core::threat_parse_stats().arena_high_water));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) {
    server_.send(500, "text/plain", "state too large\n");
    return;
  }
  server_.send(200, "application/json", text);
}
