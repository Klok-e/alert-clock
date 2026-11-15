#include "service.h"

#include <ESP8266WiFi.h>

#include "esp_platform.h"

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
    "\"threats_in_watch\":%u,\"alert_level\":\"%s\"},"
    "\"gateway\":{\"configured\":%s,\"outcome\":\"%s\",\"http\":%d,\"rejection\":\"%s\","
    "\"successes\":%lu,\"failures\":%lu,\"rejections\":%lu,\"since_valid_s\":%ld,"
    "\"alerts_stale\":%s,\"threats_stale\":%s},"
    "\"basemap\":{\"valid\":%s,\"version\":%lu,\"wanted\":%lu}}\n";

const char* json_bool(bool value) { return value ? "true" : "false"; }

const char kWifiForm[] PROGMEM =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>alert-clock Wi-Fi</title><h3>alert-clock Wi-Fi</h3>"
    "<form method=post action=/wifi><p>Network<br><input name=ssid maxlength=32 required>"
    "<p>Password<br><input name=password type=password maxlength=64>"
    "<p>Gateway (host or host:port)<br><input name=gateway maxlength=63>"
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
    const bool ok = app_.set_gateway(server_.arg("gateway").c_str());
    server_.send(ok ? 200 : 400, "text/plain", ok ? "ok\n" : "invalid gateway address\n");
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
  const String& gateway = server_.arg("gateway");
  const bool valid = ssid.length() >= 1 && ssid.length() <= 32 &&
                     (password.length() == 0 || (password.length() >= 8 && password.length() <= 64)) &&
                     gateway.length() <= gateway_store::kMaxAddress;
  if (!valid) {
    server_.send(400, "text/plain", "ssid 1-32 chars; password empty or 8-64 chars; gateway up to 63\n");
    return;
  }
  // An empty gateway field keeps the saved address.
  if (gateway.length() > 0 && (!app_.set_gateway(gateway.c_str()) || !gateway_store::save(gateway.c_str()))) {
    server_.send(400, "text/plain", "invalid gateway address\n");
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
  const core::GatewayStatus& gateway = app_.status();
  const core::Snapshot& snapshot = app_.snapshot();
  const uint32_t since = app_.since_valid_s();
  char text[kStateBytes];
  const int length = snprintf_P(
      text, sizeof(text), kStateFormat, static_cast<unsigned long>(millis() / 1000),
      json_bool(safe_mode_.active()), static_cast<unsigned long>(safe_mode_.crash_boots()),
      reset != nullptr ? static_cast<int>(reset->reason) : -1,
      static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxFreeBlockSize()),
      static_cast<unsigned>(ESP.getHeapFragmentation()), json_bool(WiFi.status() == WL_CONNECTED),
      static_cast<int>(WiFi.RSSI()), core::panel_name(view_.panel), core::banner_name(view_.banner),
      json_bool(view_.time_valid), static_cast<unsigned>(snapshot.markers.size()),
      static_cast<unsigned>(snapshot.threats_in_watch), core::alert_level_name(snapshot.alert_level),
      json_bool(app_.gateway_configured()), core::fetch_outcome_name(gateway.last_outcome),
      gateway.last_http_status, core::wire_result_name(gateway.last_rejection),
      static_cast<unsigned long>(gateway.successes), static_cast<unsigned long>(gateway.failures),
      static_cast<unsigned long>(gateway.rejections), since == core::kNever ? -1L : static_cast<long>(since),
      json_bool(snapshot.alerts_stale), json_bool(snapshot.threats_stale), json_bool(app_.basemap().valid()),
      static_cast<unsigned long>(app_.basemap().version()), static_cast<unsigned long>(snapshot.basemap_version));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) {
    server_.send(500, "text/plain", "state too large\n");
    return;
  }
  server_.send(200, "application/json", text);
}
