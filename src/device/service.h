#pragma once

#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>

#include "app.h"
#include "safe_mode.h"
#include "view.h"

// Web service for Wi-Fi setup, firmware updates and hardware-in-the-loop testing.
// Every route except "/" requires HTTP basic auth as "admin" with the admin password.
//   GET  /wifi, POST /wifi?ssid=..&password=..  stores Wi-Fi credentials and restarts
//   GET  /debug/state                  JSON: heap, Wi-Fi, current screen, source health
//   POST /debug/press?kind=short|long  simulates the button
//   POST /debug/refresh                fetches both sources now
//   POST /debug/sources?threats=URL&alerts=URL   repoints sources until reboot
//   POST /update                       firmware upload (multipart field "firmware")
class Service {
 public:
  Service(core::App& app, const core::ViewModel& view, const SafeMode& safe_mode)
      : app_(app), view_(view), safe_mode_(safe_mode) {}

  void begin(const char* admin_password);
  void handle();

 private:
  bool authorized();
  void send_state();
  void save_wifi();

  ESP8266WebServer server_{80};
  ESP8266HTTPUpdateServer updater_;
  core::App& app_;
  const core::ViewModel& view_;
  const SafeMode& safe_mode_;
  const char* password_ = "";
  uint32_t restart_at_ms_ = 0;  // non-zero: restart once millis() passes it
};
