#include "service.h"

#include <ESP8266WiFi.h>

#include "device_store.h"
#include "flash.h"

namespace {

constexpr const char* kUser = "admin";
constexpr uint32_t kRestartDelayMs = 1500;
constexpr size_t kStateBytes = 1280;

template <typename... Args>
bool append_flash(char* out, size_t capacity, size_t& used, const char* format_flash, Args... args) {
  if (used >= capacity) {
    return false;
  }
  const size_t remaining = capacity - used;
  const int length = ui::flash_format(out + used, remaining, format_flash, args...);
  if (length < 0 || static_cast<size_t>(length) >= remaining) {
    return false;
  }
  used += static_cast<size_t>(length);
  return true;
}

bool append_transfer(char* out, size_t capacity, size_t& used, const core::TransferStatus& transfer) {
  static const char kTransferFormat[] PROGMEM =
      "\"outcome\":\"%s\",\"http\":%d,\"rejection\":\"%s\","
      "\"read_end\":\"%s\",\"expected_bytes\":%ld,\"received_bytes\":%lu,\"duration_ms\":%lu";
  return append_flash(out, capacity, used, kTransferFormat, core::fetch_outcome_name(transfer.outcome),
                      transfer.http_status, core::wire_result_name(transfer.rejection),
                      core::read_end_name(transfer.read_end), static_cast<long>(transfer.expected_bytes),
                      static_cast<unsigned long>(transfer.received_bytes),
                      static_cast<unsigned long>(transfer.duration_ms));
}

const char* json_bool(bool value) { return value ? "true" : "false"; }

const char kRoutes[] PROGMEM =
    "alert-clock: /settings /wifi /debug/state /debug/press /debug/refresh /debug/sources /update\n";

const char kWifiForm[] PROGMEM =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>alert-clock Wi-Fi</title><h3>alert-clock Wi-Fi</h3>"
    "<form method=post action=/wifi><p>Network<br><input name=ssid maxlength=32 required>"
    "<p>Password<br><input name=password type=password maxlength=64>"
    "<p>Gateway (host or host:port)<br><input name=gateway maxlength=63>"
    "<p><button>Save and restart</button></form>";


const char kSettingsPage[] PROGMEM = R"HTML(<!doctype html><html lang="uk"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Налаштування годинника</title>
<style>html{color-scheme:dark light;font:16px system-ui}body{max-width:40rem;margin:2rem auto;padding:0 1rem}fieldset{margin:1rem 0;padding:1rem}label{display:block;margin:.7rem 0}input[type=number]{width:5rem}button,select{font:inherit;margin:.3rem;padding:.4rem}output{display:block;min-height:2rem}small{display:block}a{color:inherit}</style>
<h1>Світло та звук</h1><p id="profile"></p><form id="settings">
<fieldset><legend>Нічний розклад</legend><label>Початок <input type="time" name="night_start" required></label><label>Кінець <input type="time" name="night_end" required></label><small>Місцевий час Києва. До синхронізації часу діє нічний режим.</small></fieldset>
<fieldset><legend>Звук</legend><label><input type="checkbox" id="sound_on"> Звукові сповіщення</label><small>Вимкнення глушить усі автоматичні сигнали вдень і вночі.</small></fieldset>
<div id="profiles"></div><button type="submit" id="save">Зберегти</button></form>
<p>Перевірка використовує незбережені значення. Перевірка звуку чутна навіть у нічному режимі та з вимкненим сповіщенням чи звуком.</p><output id="message" role="status"></output>
<p><a href="/wifi">Wi-Fi</a></p>
<script>
const form = document.querySelector('#settings');
const message = document.querySelector('#message');
const sounds = [['raised', 'Початок тривоги', 1], ['escalated', 'Посилення тривоги', 2], ['cleared', 'Відбій', 4]];
const targets = [
  ['screen', 'Яскравість екрана'],
  ['led_yellow', 'LED: загроза'], ['led_red', 'LED: тривога'],
  ['led_nodata', 'LED: немає даних'], ['led_clear', 'LED: відбій'],
  ...sounds.map(([key, title]) => [key, 'Звук: ' + title])
];
for (const [key, title] of [['day', 'День'], ['night', 'Ніч']]) {
  const fieldset = document.createElement('fieldset');
  fieldset.innerHTML = `<legend>${title}</legend>
    <label>Екран, % <input type="number" name="${key}_screen" min="0" max="100" required></label>
    <label>Інтенсивність LED, % <input type="number" name="${key}_led" min="0" max="100" required></label>
    <p>Звуковий сигнал під час:</p>
    <small>Лише звук: екран і LED показують тривогу й відбій завжди.</small>`
    + sounds.map(([sound, label]) => `<label><input type="checkbox" id="${key}_${sound}"> ${label}</label>`).join('')
    + `<label>Гучність сигналу, % <input type="number" name="${key}_volume" min="1" max="100" required></label>`
    + `<label>Перевірити <select id="${key}_target">`
    + targets.map(([target, label]) => `<option value="${target}">${label}</option>`).join('')
    + `</select></label><button type="button" data-preview="${key}" disabled>Перевірити</button>`;
  document.querySelector('#profiles').append(fieldset);
}
let pending = false, loaded = false;
// A reply lost on Wi-Fi must not leave the buttons disabled until a reload.
async function request(url, options = {}) {
  const abort = new AbortController();
  const timer = setTimeout(() => abort.abort(), 8000);
  try { return await fetch(url, {...options, cache: 'no-store', signal: abort.signal}); }
  catch (error) { throw Error('Годинник не відповів. Спробуйте ще раз.'); }
  finally { clearTimeout(timer); }
}
function soundMask(profile) {
  return sounds.reduce((mask, [key, , bit]) =>
    mask | (document.getElementById(profile + '_' + key).checked ? bit : 0), 0);
}
function previewButtons(unavailable) {
  document.querySelectorAll('[data-preview]').forEach(button => {
    button.disabled = !loaded || pending || unavailable;
  });
}
async function state(fill = false) {
  const response = await request('/settings/state');
  if (!response.ok) throw Error('Не вдалося прочитати налаштування');
  const settings = await response.json();
  if (fill) {
    for (const profile of ['day', 'night']) {
      for (const field of ['screen', 'led', 'volume']) {
        form.elements[profile + '_' + field].value = settings[profile + '_' + field];
      }
      for (const [key, , bit] of sounds) {
        document.getElementById(profile + '_' + key).checked = !!(settings[profile + '_sounds'] & bit);
      }
    }
    for (const key of ['night_start', 'night_end']) form.elements[key].value = settings[key];
    document.getElementById('sound_on').checked = settings.sound_on;
    loaded = true;
  }
  document.querySelector('#profile').textContent = settings.safe_mode
    ? 'Режим відновлення: перевірки недоступні.'
    : 'Зараз діє ' + (settings.active_profile === 'night' ? 'нічний' : 'денний') + ' режим.';
  previewButtons(settings.safe_mode);
}
async function post(url, data) {
  const response = await request(url, {method: 'POST', body: new URLSearchParams(data)});
  if (!response.ok) {
    throw Error(response.status === 409 ? 'Зачекайте завершення поточного сигналу.'
      : response.status === 400 ? 'Перевірте значення та нічний розклад.'
      : 'Не вдалося зберегти або застосувати налаштування.');
  }
}
async function finishAction() {
  pending = false;
  try { await state(); }
  catch (error) { message.textContent = error.message; previewButtons(false); }
}
form.addEventListener('submit', async event => {
  event.preventDefault();
  if (!loaded || !form.reportValidity() || pending) return;
  pending = true;
  form.querySelector('#save').disabled = true;
  previewButtons(true);
  try {
    const data = {};
    for (const profile of ['day', 'night']) {
      for (const field of ['screen', 'led', 'volume']) data[profile + '_' + field] = form.elements[profile + '_' + field].value;
      data[profile + '_sounds'] = soundMask(profile);
    }
    for (const key of ['night_start', 'night_end']) data[key] = form.elements[key].value;
    data.sound_on = document.getElementById('sound_on').checked ? 1 : 0;
    await post('/settings', data);
    message.textContent = 'Збережено.';
  } catch (error) {
    message.textContent = error.message;
  } finally {
    form.querySelector('#save').disabled = false;
    await finishAction();
  }
});
for (const button of document.querySelectorAll('[data-preview]')) {
  button.addEventListener('click', async () => {
    if (!loaded || !form.reportValidity() || pending) return;
    pending = true;
    previewButtons(true);
    const profile = button.dataset.preview;
    try {
      await post('/settings/preview', {
        profile, target: document.getElementById(profile + '_target').value,
        screen: form.elements[profile + '_screen'].value,
        led: form.elements[profile + '_led'].value, sounds: soundMask(profile),
        volume: form.elements[profile + '_volume'].value
      });
      message.textContent = 'Перевірка триває.';
    } catch (error) {
      message.textContent = error.message;
    } finally {
      await finishAction();
    }
  });
}
form.querySelector('#save').disabled = true;
state(true).then(() => form.querySelector('#save').disabled = false)
  .catch(error => message.textContent = error.message);
</script></html>)HTML";

bool parse_percent(const String& text, uint8_t maximum, uint8_t& result) {
  if (text.length() == 0 || text.length() > 3) return false;
  unsigned value = 0;
  for (unsigned i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + static_cast<unsigned>(text[i] - '0');
  }
  if (value > maximum) return false;
  result = static_cast<uint8_t>(value);
  return true;
}

bool parse_minute(const String& text, uint16_t& result) {
  if (text.length() != 5 || text[2] != ':') return false;
  for (unsigned i : {0U, 1U, 3U, 4U}) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  const unsigned hour = static_cast<unsigned>((text[0] - '0') * 10 + text[1] - '0');
  const unsigned minute = static_cast<unsigned>((text[3] - '0') * 10 + text[4] - '0');
  if (hour >= 24 || minute >= 60) return false;
  result = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

bool parse_target(const String& text, core::PreviewTarget& target) {
  static const char* const names[] = {"screen", "led_yellow", "led_red", "led_nodata",
                                     "led_clear", "raised", "escalated", "cleared"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (text == names[i]) {
      target = static_cast<core::PreviewTarget>(i);
      return true;
    }
  }
  return false;
}

}  // namespace

void Service::begin(const char* admin_password) {
  password_ = admin_password;

  server_.on("/", HTTP_GET, [this]() {
    server_.send_P(200, "text/plain", kRoutes);
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
  server_.on("/settings", HTTP_GET, [this]() {
    if (!authorized()) return;
    // A page kept from older firmware would post fields this firmware rejects.
    server_.sendHeader("Cache-Control", "no-store");
    server_.send_P(200, "text/html; charset=utf-8", kSettingsPage);
  });
  server_.on("/settings/state", HTTP_GET, [this]() {
    if (authorized()) send_settings();
  });
  server_.on("/settings", HTTP_POST, [this]() {
    if (authorized()) save_settings();
  });
  server_.on("/settings/preview", HTTP_POST, [this]() {
    if (authorized()) preview_settings();
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
  if (gateway.length() > 0) {
    if (!core::valid_gateway_address(gateway.c_str())) {
      server_.send(400, "text/plain", "invalid gateway address\n");
      return;
    }
    if (!gateway_store::save(gateway.c_str())) {
      server_.send(500, "text/plain", "gateway not saved\n");
      return;
    }
    app_.set_gateway(gateway.c_str());
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
  const core::BasemapStatus& basemap_status = app_.basemap_status();
  const core::Snapshot& snapshot = app_.snapshot();
  const uint32_t since = app_.since_valid_s();
  char text[kStateBytes];
  size_t used = 0;

  // Values are numbers, booleans or fixed enum names; none need JSON escaping.
  static const char kPrefixFormat[] PROGMEM =
      "{\"uptime_s\":%lu,\"safe_mode\":%s,\"crash_boots\":%lu,\"reset_reason\":%d,";
  bool ok = append_flash(
      text, sizeof(text), used, kPrefixFormat, static_cast<unsigned long>(millis() / 1000),
      json_bool(safe_mode_.active()), static_cast<unsigned long>(safe_mode_.crash_boots()),
      reset != nullptr ? static_cast<int>(reset->reason) : -1);

  static const char kHeapFormat[] PROGMEM =
      "\"heap\":{\"free\":%lu,\"max_block\":%lu,\"fragmentation\":%u},";
  ok = ok && append_flash(
      text, sizeof(text), used, kHeapFormat,
      static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxFreeBlockSize()),
      static_cast<unsigned>(ESP.getHeapFragmentation()));

  static const char kWifiFormat[] PROGMEM = "\"wifi\":{\"connected\":%s,\"rssi\":%d},";
  ok = ok && append_flash(text, sizeof(text), used, kWifiFormat,
                          json_bool(WiFi.status() == WL_CONNECTED), static_cast<int>(WiFi.RSSI()));

  static const char kViewFormat[] PROGMEM =
      "\"view\":{\"panel\":\"%s\",\"banner\":\"%s\",\"time_valid\":%s,\"markers\":%u,"
      "\"threats_in_watch\":%u,\"alert_level\":\"%s\",\"alert_duration_s\":%ld},";
  ok = ok && append_flash(
      text, sizeof(text), used, kViewFormat, core::panel_name(view_.panel), core::banner_name(view_.banner),
      json_bool(view_.time_valid), static_cast<unsigned>(snapshot.markers.size()),
      static_cast<unsigned>(snapshot.threats_in_watch), core::alert_level_name(snapshot.alert_level),
      view_.alert_duration_s == core::kNever ? -1L : static_cast<long>(view_.alert_duration_s));

  static const char kGatewayFormat[] PROGMEM =
      "\"gateway\":{\"configured\":%s,\"outcome\":\"%s\",\"http\":%d,\"rejection\":\"%s\","
      "\"successes\":%lu,\"failures\":%lu,\"rejections\":%lu,\"since_valid_s\":%ld,"
      "\"alerts_stale\":%s,\"threats_stale\":%s},";
  ok = ok && append_flash(
      text, sizeof(text), used, kGatewayFormat,
      json_bool(app_.gateway_configured()), core::fetch_outcome_name(gateway.last_outcome),
      gateway.last_http_status, core::wire_result_name(gateway.last_rejection),
      static_cast<unsigned long>(gateway.successes), static_cast<unsigned long>(gateway.failures),
      static_cast<unsigned long>(gateway.rejections), since == core::kNever ? -1L : static_cast<long>(since),
      json_bool(snapshot.alerts_stale), json_bool(snapshot.threats_stale));

  static const char kBasemapFormat[] PROGMEM = "\"basemap\":{\"valid\":%s,\"version\":%lu,\"wanted\":%lu,";
  ok = ok && append_flash(text, sizeof(text), used, kBasemapFormat, json_bool(app_.basemap().valid()),
                          static_cast<unsigned long>(app_.basemap().version()),
                          static_cast<unsigned long>(snapshot.basemap_version));
  ok = ok && append_transfer(text, sizeof(text), used, basemap_status.last);

  static const char kBasemapCountersFormat[] PROGMEM =
      ",\"successes\":%lu,\"failures\":%lu,\"last_failure\":{";
  ok = ok && append_flash(text, sizeof(text), used, kBasemapCountersFormat,
                          static_cast<unsigned long>(basemap_status.successes),
                          static_cast<unsigned long>(basemap_status.failures));
  ok = ok && append_transfer(text, sizeof(text), used, basemap_status.last_failure);
  static const char kOutputsFormat[] PROGMEM =
      "}},\"outputs\":{\"night\":%s,\"screen_percent\":%u,\"frequency_hz\":%u,\"volume_percent\":%u,"
      "\"led_rgb\":%lu,\"preview_busy\":%s}}\n";
  ok = ok && append_flash(text, sizeof(text), used, kOutputsFormat, json_bool(outputs_.night()),
                          static_cast<unsigned>(outputs_.screen_percent()),
                          static_cast<unsigned>(outputs_.frequency_hz()),
                          static_cast<unsigned>(outputs_.volume_percent()),
                          static_cast<unsigned long>(outputs_.led_rgb()), json_bool(outputs_.preview_busy()));
  if (!ok) {
    server_.send(500, "text/plain", "state too large\n");
    return;
  }
  server_.send(200, "application/json", text);
}

bool Service::fields_match(const char* const* names, size_t count) {
  if (static_cast<size_t>(server_.args()) != count) return false;
  for (size_t i = 0; i < count; ++i) {
    unsigned matches = 0;
    for (int j = 0; j < server_.args(); ++j) {
      if (server_.argName(j) == names[i]) ++matches;
    }
    if (matches != 1) return false;
  }
  return true;
}

void Service::send_settings() {
  const core::OutputSettings& settings = outputs_.settings();
  static const char kFormat[] PROGMEM =
      "{\"day_screen\":%u,\"day_led\":%u,\"day_sounds\":%u,\"day_volume\":%u,"
      "\"night_screen\":%u,\"night_led\":%u,\"night_sounds\":%u,\"night_volume\":%u,"
      "\"sound_on\":%s,"
      "\"night_start\":\"%02u:%02u\",\"night_end\":\"%02u:%02u\","
      "\"active_profile\":\"%s\",\"preview_busy\":%s,\"safe_mode\":%s}\n";
  char text[320];
  const int length = ui::flash_format(
      text, sizeof(text), kFormat, static_cast<unsigned>(settings.day.screen_percent),
      static_cast<unsigned>(settings.day.led_percent), static_cast<unsigned>(settings.day.sound_mask),
      static_cast<unsigned>(settings.day.volume_percent),
      static_cast<unsigned>(settings.night.screen_percent), static_cast<unsigned>(settings.night.led_percent),
      static_cast<unsigned>(settings.night.sound_mask), static_cast<unsigned>(settings.night.volume_percent),
      json_bool(settings.sound_on), static_cast<unsigned>(settings.night_start / 60),
      static_cast<unsigned>(settings.night_start % 60), static_cast<unsigned>(settings.night_end / 60),
      static_cast<unsigned>(settings.night_end % 60), outputs_.night() ? "night" : "day",
      json_bool(outputs_.preview_busy()), json_bool(safe_mode_.active()));
  if (length < 0 || static_cast<size_t>(length) >= sizeof(text)) {
    server_.send(500, "text/plain", "state too large\n");
    return;
  }
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", text);
}

void Service::save_settings() {
  const char* const names[] = {"day_screen", "day_led", "day_sounds", "day_volume", "night_screen",
                               "night_led", "night_sounds", "night_volume", "sound_on", "night_start",
                               "night_end"};
  core::OutputSettings settings;
  uint8_t sound_on = 0;
  const bool valid = fields_match(names, sizeof(names) / sizeof(names[0])) &&
      parse_percent(server_.arg("day_screen"), 100, settings.day.screen_percent) &&
      parse_percent(server_.arg("day_led"), 100, settings.day.led_percent) &&
      parse_percent(server_.arg("day_sounds"), 7, settings.day.sound_mask) &&
      parse_percent(server_.arg("day_volume"), 100, settings.day.volume_percent) &&
      parse_percent(server_.arg("night_screen"), 100, settings.night.screen_percent) &&
      parse_percent(server_.arg("night_led"), 100, settings.night.led_percent) &&
      parse_percent(server_.arg("night_sounds"), 7, settings.night.sound_mask) &&
      parse_percent(server_.arg("night_volume"), 100, settings.night.volume_percent) &&
      parse_percent(server_.arg("sound_on"), 1, sound_on) &&
      parse_minute(server_.arg("night_start"), settings.night_start) &&
      parse_minute(server_.arg("night_end"), settings.night_end);
  settings.sound_on = sound_on == 1;
  if (!valid || !core::valid_output_settings(settings)) {
    server_.send(400, "text/plain", "invalid output settings\n");
    return;
  }
  if (!output_store::save(settings)) {
    server_.send(500, "text/plain", "settings not saved\n");
    return;
  }
  outputs_.configure(settings, millis());
  server_.send(200, "text/plain", "saved\n");
}

void Service::preview_settings() {
  const char* const names[] = {"profile", "target", "screen", "led", "sounds", "volume"};
  core::OutputProfile profile;
  core::PreviewTarget target = core::PreviewTarget::Screen;
  const String& name = server_.arg("profile");
  const bool valid = fields_match(names, sizeof(names) / sizeof(names[0])) &&
      (name == "day" || name == "night") && parse_target(server_.arg("target"), target) &&
      parse_percent(server_.arg("screen"), 100, profile.screen_percent) &&
      parse_percent(server_.arg("led"), 100, profile.led_percent) &&
      parse_percent(server_.arg("sounds"), 7, profile.sound_mask) &&
      parse_percent(server_.arg("volume"), 100, profile.volume_percent);
  if (!valid) {
    server_.send(400, "text/plain", "invalid preview\n");
    return;
  }
  if (safe_mode_.active() || !outputs_.preview(profile, target, millis())) {
    server_.send(409, "text/plain", "outputs busy or unavailable\n");
    return;
  }
  server_.send(202, "text/plain", "preview started\n");
}
