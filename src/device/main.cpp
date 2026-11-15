#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <time.h>

#include <etl/error_handler.h>

#include "app.h"
#include "board.h"
#include "config.h"
#include "esp_platform.h"
#include "outputs.h"
#include "safe_mode.h"
#include "scene.h"
#include "service.h"

namespace {

constexpr const char* kTimeZone = "EET-2EEST,M3.5.0/3,M10.5.0/4";  // Europe/Kyiv
constexpr const char* kSetupNetwork = "AlertClock-Setup";
constexpr uint32_t kWifiWaitMs = 20000;
constexpr uint32_t kFrameMs = 1000;
constexpr uint32_t kHealthyAfterMs = 60000;
constexpr uint8_t kRotation = 0;    // check on the device; the supplier stores it in EEPROM
constexpr uint8_t kBrightness = 80;  // percent

core::Settings make_settings() {
  core::Settings settings;
  settings.home = core::LatLon{CLOCK_HOME_LAT, CLOCK_HOME_LON};
  settings.home_region = CLOCK_HOME_REGION;
  settings.map_radius_km = CLOCK_MAP_RADIUS_KM;
  settings.watch_radius_km = CLOCK_WATCH_RADIUS_KM;
  settings.alerts_url =
      "https://api.alerts.in.ua/v2/iot/active_air_raid_alerts/" CLOCK_ALERTS_REGION_UID ".json";
  settings.alerts_token = CLOCK_ALERTS_TOKEN;
  return settings;
}

static_assert(sizeof(CLOCK_ADMIN_PASSWORD) - 1 >= 8,
              "CLOCK_ADMIN_PASSWORD must have 8+ characters: it is also the setup network's WPA2 key");

// Everything is allocated statically or once in setup(); the loop does not allocate.
ClockDisplay display;
uint8_t band_pixels[core::kScreenPx * ui::kBandHeight];  // RGB332
lgfx::LGFX_Sprite band(&display);
EspTime time_source;
EspTransport transport;
core::App app(make_settings(), time_source, transport);
core::ViewModel view;
core::ButtonDetector button;
Outputs outputs;
SafeMode safe_mode;
Service service(app, view, safe_mode);
uint32_t last_frame_ms = 0;
bool setup_network_up = false;

// Container overflows and out-of-range indexes end here instead of corrupting memory.
void on_etl_error(const etl::exception& error) {
  safe_mode.record_crash();
  Serial.printf("ETL check failed: %s (%s:%d)\n", error.what(), error.file_name(),
                static_cast<int>(error.line_number()));
  Serial.flush();
  abort();
}

void show_message(const char* title, const char* detail) {
  display.startWrite();
  display.fillScreen(0);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextColor(0xFFFF);
  display.setFont(&lgfx::fonts::FreeSansBold9pt7b);
  display.drawString(title, core::kScreenPx / 2, 100);
  display.setFont(&lgfx::fonts::Font2);
  display.drawString(detail, core::kScreenPx / 2, 135);
  display.endWrite();
}

bool wait_for_wifi(uint32_t timeout_ms) {
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= timeout_ms) {
      return false;
    }
    delay(100);
  }
  return true;
}

// Joins the network saved in flash. Without one, opens the WPA2 setup network
// (key: the admin password); its page http://192.168.4.1/wifi saves new credentials.
void connect_wifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  if (wait_for_wifi(kWifiWaitMs)) {
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  setup_network_up = WiFi.softAP(kSetupNetwork, CLOCK_ADMIN_PASSWORD);
  show_message("Wi-Fi setup", "AlertClock-Setup 192.168.4.1/wifi");
}

// Shows the clock's address on screen and the serial log; uploads and /debug need it.
void show_address(const char* title) {
  char text[24];
  const IPAddress ip = WiFi.localIP();
  snprintf(text, sizeof(text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  show_message(title, text);
  Serial.printf("%s: %s\n", title, text);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  etl::set_assert_function(on_etl_error);
  safe_mode.begin();
  outputs.begin();

  display.init();
  display.setRotation(kRotation);
  display.fillScreen(0);
  outputs.set_backlight(kBrightness);
  band.setBuffer(band_pixels, core::kScreenPx, ui::kBandHeight, 8);

  show_message("Connecting Wi-Fi", "");
  connect_wifi();
  configTime(kTimeZone, "pool.ntp.org", "time.google.com");
  transport.begin();
  service.begin(CLOCK_ADMIN_PASSWORD);

  if (safe_mode.active()) {
    show_address("SAFE MODE");
    Serial.println("Safe mode after repeated crashes: only Wi-Fi, firmware upload and /debug run.");
  } else if (WiFi.status() == WL_CONNECTED) {
    show_address("Wi-Fi connected");
    delay(1500);  // long enough to read the address
  }
}

void loop() {
  service.handle();
  if (safe_mode.active()) {
    delay(5);
    return;
  }
  if (setup_network_up && WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(true);  // the saved network is back
    setup_network_up = false;
  }
  const uint32_t now = millis();
  if (now >= kHealthyAfterMs) {
    safe_mode.mark_healthy();
  }

  const core::ButtonEvent event = button.update(digitalRead(board::kButtonPin) == LOW, now);
  if (event != core::ButtonEvent::None) {
    app.press(event);
    last_frame_ms = now - kFrameMs;  // redraw right away
  }

  // A fetch blocks the loop for seconds and only update() ends a note, so a pattern
  // finishes (at most ~2 s) before the next fetch starts.
  if (!outputs.busy()) {
    app.tick();
  }
  outputs.play(app.take_sound());

  const uint32_t after = millis();
  if (after - last_frame_ms >= kFrameMs) {
    last_frame_ms = after;
    app.build_view(view);
    ui::render(display, band, view);
    outputs.show_banner(view.banner);
  }
  outputs.update(after);
}
