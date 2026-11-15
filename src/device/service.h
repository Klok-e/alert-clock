#pragma once

#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>

#include "app.h"
#include "safe_mode.h"
#include "output_policy.h"
#include "view.h"

// Web service for Wi-Fi setup, firmware updates and hardware-in-the-loop testing.
// Every route except "/" requires HTTP basic auth as "admin" with the admin password.
//   GET  /wifi, POST /wifi?ssid=..&password=..&gateway=host[:port]
//                                      stores Wi-Fi credentials and the gateway address; restarts
//   GET  /debug/state                  JSON: heap, Wi-Fi, current screen, gateway health
//   POST /debug/press?kind=short|long  simulates the button
//   POST /debug/refresh                polls the gateway now
//   POST /debug/sources?gateway=host[:port]   points the clock at another gateway until reboot
//   GET /settings, GET /settings/state, POST /settings, POST /settings/preview
//                                      output profiles, schedule and temporary previews
//   POST /update                       firmware upload (multipart field "firmware")
class Service {
 public:
  Service(core::App& app, const core::ViewModel& view, const SafeMode& safe_mode,
          core::OutputController& outputs)
      : app_(app), view_(view), safe_mode_(safe_mode), outputs_(outputs) {}

  void begin(const char* admin_password);
  void handle();

 private:
  bool authorized();
  void send_state();
  void save_wifi();
  void send_settings();
  void save_settings();
  void preview_settings();
  bool fields_match(const char* const* names, size_t count);

  ESP8266WebServer server_{80};
  ESP8266HTTPUpdateServer updater_;
  core::App& app_;
  const core::ViewModel& view_;
  const SafeMode& safe_mode_;
  core::OutputController& outputs_;
  const char* password_ = "";
  uint32_t restart_at_ms_ = 0;  // non-zero: restart once millis() passes it
};
