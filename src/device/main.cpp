#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <time.h>

#include <etl/error_handler.h>

#include "app.h"
#include "board.h"
#include "config.h"
#include "esp_platform.h"
#include "device_store.h"
#include "flash.h"
#include "fonts.h"
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

constexpr uint32_t kNoticeMs = 4000;

core::Settings make_settings() {
  core::Settings settings;
  core::parse_key_hex(CLOCK_GATEWAY_KEY, settings.key);
  return settings;
}

static_assert(sizeof(CLOCK_GATEWAY_KEY) - 1 == 64, "CLOCK_GATEWAY_KEY must be 64 hex digits");
static_assert(sizeof(CLOCK_ADMIN_PASSWORD) - 1 >= 8,
              "CLOCK_ADMIN_PASSWORD must have 8+ characters: it is also the setup network's WPA2 key");

// Everything is allocated statically or once in setup(); the loop does not allocate.
ClockDisplay display;
uint16_t band_pixels[core::kScreenPx * ui::kBandHeight];  // RGB565
lgfx::LGFX_Sprite band(&display);
EspTime time_source;
EspTransport transport;
EspRandom random_source;
core::App app(make_settings(), time_source, transport, random_source);
core::ViewModel view;
core::ButtonDetector button;
Outputs outputs;
core::OutputController output_controller(outputs);
SafeMode safe_mode;
Service service(app, view, safe_mode, output_controller);
uint32_t last_frame_ms = 0;
uint32_t notice_from_ms = 0;  // the start-up notice shows for kNoticeMs from here
bool notice_done = false;
bool setup_network_up = false;

// Container overflows and out-of-range indexes end here instead of corrupting memory.
void on_etl_error(const etl::exception& error) {
  safe_mode.record_crash();
  Serial.printf("ETL check failed: %s (%s:%d)\n", error.what(), error.file_name(),
                static_cast<int>(error.line_number()));
  Serial.flush();
  abort();
}

// Start-up messages, in flash (plain literals occupy RAM on the ESP8266).
const char kConnecting[] PROGMEM = "Підключення до Wi-Fi";
const char kSetupTitle[] PROGMEM = "Налаштування Wi-Fi";
const char kSetupDetail[] PROGMEM = "AlertClock-Setup 192.168.4.1/wifi";
const char kConnected[] PROGMEM = "Wi-Fi підключено";
const char kSafeMode[] PROGMEM = "Безпечний режим";

void show_message(const char* title_flash, const char* detail_ram) {
  char text[64];
  ui::flash_copy(text, sizeof(text), title_flash);
  display.startWrite();
  display.fillScreen(0);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextColor(0xFFFF);
  display.setFont(&ui::kInterTitle);
  display.drawString(text, core::kScreenPx / 2, 100);
  display.setFont(&ui::kInterBody);
  display.drawString(detail_ram, core::kScreenPx / 2, 135);
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
  char detail[48];
  ui::flash_copy(detail, sizeof(detail), kSetupDetail);
  show_message(kSetupTitle, detail);
}

// Shows the clock's address on screen and the serial log; uploads and /debug need it.
void show_address(const char* title_flash) {
  char text[24];
  const IPAddress ip = WiFi.localIP();
  snprintf(text, sizeof(text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  show_message(title_flash, text);
  char title_text[48];
  ui::flash_copy(title_text, sizeof(title_text), title_flash);
  Serial.printf("%s: %s\n", title_text, text);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  etl::set_assert_function(on_etl_error);
  safe_mode.begin();
  outputs.begin();

  display.init();
  // SPI initialization claims GPIO12 as MISO; the LED must reclaim it afterward.
  outputs.begin_led();
  display.setRotation(kRotation);
  display.fillScreen(0);
  gateway_store::begin();
  output_controller.configure(output_store::load(), millis());
  output_controller.observe(false, 0, core::Banner::NoData, core::Sound::None, millis());
  band.setBuffer(band_pixels, core::kScreenPx, ui::kBandHeight, 16);

  show_message(kConnecting, "");
  connect_wifi();
  configTime(kTimeZone, "pool.ntp.org", "time.google.com");
  if (!app.set_gateway(gateway_store::load())) {
    Serial.println("No gateway address: set it on /wifi.");
  }
  service.begin(CLOCK_ADMIN_PASSWORD);

  if (safe_mode.active()) {
    show_address(kSafeMode);
    Serial.println("Safe mode after repeated crashes: only Wi-Fi, firmware upload and /debug run.");
  } else if (WiFi.status() == WL_CONNECTED) {
    show_address(kConnected);
    delay(1500);  // long enough to read the address
  }
  notice_from_ms = millis();
  last_frame_ms = notice_from_ms - kFrameMs;
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

  app.tick();
  output_controller.gateway_advanced();
  app.build_view(view);
  const uint32_t after = millis();
  output_controller.observe(view.time_valid,
      static_cast<uint16_t>(view.local.hour * 60 + view.local.minute),
      view.banner, app.take_sound(), after);
  if (after - last_frame_ms >= kFrameMs) {
    last_frame_ms = after;
    if (!notice_done && after - notice_from_ms >= kNoticeMs) {
      notice_done = true;
    }
    view.notice = !notice_done;
    ui::render(display, band, view);
  }
  output_controller.update(millis());
}
