// Scenario program: drives the clock's App against a real gateway (fed by the mock server)
// with a scripted clock and button, renders every step through the same UI code as the
// device, and checks each step's expectations. tools/run_scenarios.py starts both servers.
//
//   program --scenario FILE --gateway HOST:PORT --key HEX64 --mock URL --out DIR
//
// Before each step it switches the mock's phase (when the step names one) and sets the
// gateway's clock to the step's time through /_test/time, which also makes the gateway
// fetch from the mock at once. Writes DIR/NN-<label>.png and DIR/states.jsonl. Exit status:
// 0 when every expectation held, 1 when one failed, 2 on bad input.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <set>

#include "gfx.h"

#include "app.h"
#include "host_platform.h"
#include "json.h"
#include "scene.h"
#include "output_policy.h"
#include "time_util.h"

namespace {

constexpr const char* kTimeZone = "EET-2EEST,M3.5.0/3,M10.5.0/4";  // Europe/Kyiv
class TraceOutputs final : public core::OutputSink {
 public:
  explicit TraceOutputs(std::ofstream& out) : out_(out) {}
  void begin_step(int step, uint32_t now) {
    step_ = step;
    now_ = now;
    led_changes = 0;
    led_changes_fetching = 0;
    frequencies.clear();
    frequencies.insert(frequency);
    screens.clear();
    screens.insert(screen);
    first_buzzer_ms = UINT32_MAX;
    step_start_ = now;
  }
  void at(uint32_t now) { now_ = now; }
  void fetching(bool active) { fetching_ = active; }
  void backlight(uint8_t percent) override {
    screen = percent;
    screens.insert(percent);
    write("backlight_percent", percent);
  }
  void buzzer(uint16_t hz) override {
    frequency = hz;
    if (hz != 0 && first_buzzer_ms == UINT32_MAX) first_buzzer_ms = now_ - step_start_;
    frequencies.insert(hz);
    write("buzzer_hz", hz);
  }
  void led(uint8_t red, uint8_t green, uint8_t blue) override {
    ++led_changes;
    if (fetching_) ++led_changes_fetching;
    rgb = static_cast<uint32_t>(red) << 16 | static_cast<uint32_t>(green) << 8 | blue;
    write("led_rgb", rgb);
  }
  uint8_t screen = 0;
  uint16_t frequency = 0;
  uint32_t rgb = 0;
  int led_changes = 0;
  int led_changes_fetching = 0;
  std::set<uint16_t> frequencies;
  std::set<uint8_t> screens;
  uint32_t first_buzzer_ms = UINT32_MAX;
 private:
  void write(const char* output, uint32_t value) {
    out_ << "{\"step\":" << step_ << ",\"ms\":" << now_
         << ",\"output\":\"" << output << "\",\"value\":" << value
         << ",\"fetching\":" << (fetching_ ? "true" : "false") << "}\n";
  }
  std::ofstream& out_;
  int step_ = -1;
  uint32_t now_ = 0;
  uint32_t step_start_ = 0;
  bool fetching_ = false;
};

struct Args {
  std::string scenario;
  std::string gateway;
  std::string key;
  std::string mock;
  std::string out;
};

bool parse_args(int argc, char** argv, Args& args) {
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    std::string* target = flag == "--scenario" ? &args.scenario
                          : flag == "--gateway" ? &args.gateway
                          : flag == "--key"     ? &args.key
                          : flag == "--mock"    ? &args.mock
                          : flag == "--out"     ? &args.out
                                                : nullptr;
    if (target == nullptr) {
      return false;
    }
    *target = argv[i + 1];
  }
  return !args.scenario.empty() && !args.gateway.empty() && !args.key.empty() && !args.mock.empty() &&
         !args.out.empty();
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream text;
  text << in.rdbuf();
  out = text.str();
  return true;
}

bool write_png(lgfx::LGFX_Sprite& canvas, const std::string& path) {
  size_t length = 0;
  // createPng treats a zero width or height as empty, not "whole sprite".
  void* png = canvas.createPng(&length, 0, 0, canvas.width(), canvas.height());
  if (png == nullptr) {
    return false;
  }
  FILE* file = std::fopen(path.c_str(), "wb");
  const bool ok = file != nullptr && std::fwrite(png, 1, length, file) == length;
  if (file != nullptr) {
    std::fclose(file);
  }
  std::free(png);
  return ok;
}

// Compares one expectation; prints and counts a mismatch.
int check(int step, const char* what, const std::string& expected, const std::string& actual) {
  if (expected == actual) {
    return 0;
  }
  std::printf("FAIL step %d: %s expected %s, got %s\n", step, what, expected.c_str(), actual.c_str());
  return 1;
}

int check_expectations(int step, JsonObjectConst expect, const core::ViewModel& view, core::Sound sound,
                       const core::App& app) {
  int failures = 0;
  if (const char* panel = expect["panel"]) {
    failures += check(step, "panel", panel, core::panel_name(view.panel));
  }
  if (const char* banner = expect["banner"]) {
    failures += check(step, "banner", banner, core::banner_name(view.banner));
  }
  if (const char* expected_sound = expect["sound"]) {
    failures += check(step, "sound", expected_sound, core::sound_name(sound));
  }
  if (expect["basemap"].is<bool>()) {
    failures += check(step, "basemap", expect["basemap"].as<bool>() ? "valid" : "invalid",
                      app.basemap().valid() ? "valid" : "invalid");
  }
  if (expect["alert_minutes"].is<int>()) {
    const long minutes = view.alert_duration_s == core::kNever ? -1 : static_cast<long>(view.alert_duration_s / 60);
    failures += check(step, "alert_minutes", std::to_string(expect["alert_minutes"].as<int>()),
                      std::to_string(minutes));
  }
  const int markers = static_cast<int>(app.snapshot().markers.size());
  if (JsonObjectConst first = expect["first_marker"].as<JsonObjectConst>(); !first.isNull()) {
    if (markers == 0) {
      failures += check(step, "first_marker", "present", "absent");
    } else {
      const core::Marker& marker = app.snapshot().markers[0];
      for (const auto& field : {std::pair<const char*, int>{"x", marker.point.x},
                                {"y", marker.point.y}, {"anchor_x", marker.anchor.x},
                                {"anchor_y", marker.anchor.y}, {"trail_count", marker.trail_count}}) {
        if (first[field.first].is<int>()) {
          failures += check(step, field.first, std::to_string(first[field.first].as<int>()),
                            std::to_string(field.second));
        }
      }
      if (first["predicted"].is<bool>()) {
        failures += check(step, "predicted", first["predicted"].as<bool>() ? "yes" : "no",
                          marker.predicted ? "yes" : "no");
      }
    }
  }
  if (expect["markers_min"].is<int>() && markers < expect["markers_min"].as<int>()) {
    std::printf("FAIL step %d: markers expected >= %d, got %d\n", step, expect["markers_min"].as<int>(), markers);
    ++failures;
  }
  if (expect["markers_max"].is<int>() && markers > expect["markers_max"].as<int>()) {
    std::printf("FAIL step %d: markers expected <= %d, got %d\n", step, expect["markers_max"].as<int>(), markers);
    ++failures;
  }
  return failures;
}

void write_state(std::ofstream& out, int step, int64_t at, const std::string& label,
                 const core::ViewModel& view, core::Sound sound, const core::App& app) {
  const core::Snapshot& s = app.snapshot();
  JsonDocument line;
  line["step"] = step;
  line["at"] = static_cast<long>(at);
  line["label"] = label;
  line["panel"] = core::panel_name(view.panel);
  line["banner"] = core::banner_name(view.banner);
  line["alert_level"] = core::alert_level_name(s.alert_level);
  if (view.alert_duration_s != core::kNever) {
    line["alert_duration_s"] = view.alert_duration_s;
  }
  line["markers"] = s.markers.size();
  line["threats_in_watch"] = s.threats_in_watch;
  line["area_only_in_region"] = s.area_only_in_region;
  line["sound"] = core::sound_name(sound);
  line["outcome"] = core::fetch_outcome_name(app.status().last_outcome);
  line["rejection"] = core::wire_result_name(app.status().last_rejection);
  line["basemap_valid"] = app.basemap().valid();
  line["basemap_version"] = app.basemap().version();
  std::string text;
  serializeJson(line, text);
  out << text << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    std::fprintf(stderr, "usage: %s --scenario FILE --gateway HOST:PORT --key HEX64 --mock URL --out DIR\n",
                 argv[0]);
    return 2;
  }
  setenv("TZ", kTimeZone, 1);
  tzset();

  std::string text;
  if (!read_file(args.scenario, text)) {
    std::fprintf(stderr, "cannot read %s\n", args.scenario.c_str());
    return 2;
  }
  JsonDocument scenario;
  if (deserializeJson(scenario, text)) {
    std::fprintf(stderr, "invalid JSON in %s\n", args.scenario.c_str());
    return 2;
  }
  int64_t start = 0;
  if (!core::parse_rfc3339(scenario["start"] | "", start)) {
    std::fprintf(stderr, "scenario needs \"start\" as an RFC 3339 time\n");
    return 2;
  }

  core::Settings settings;
  if (!core::parse_key_hex(args.key.c_str(), settings.key)) {
    std::fprintf(stderr, "--key must be 64 hex digits\n");
    return 2;
  }
  FakeTime time;
  SocketTransport transport;
  HostRandom random;
  core::App app(settings, time, transport, random);
  if (!app.set_gateway(args.gateway.c_str())) {
    std::fprintf(stderr, "invalid --gateway %s\n", args.gateway.c_str());
    return 2;
  }
  const std::string gateway_time_url = "http://" + args.gateway + "/_test/time";

  lgfx::LGFX_Sprite canvas;
  canvas.setColorDepth(16);
  lgfx::LGFX_Sprite band;
  band.setColorDepth(16);  // same strip format as the clock
  if (canvas.createSprite(core::kScreenPx, core::kScreenPx) == nullptr ||
      band.createSprite(core::kScreenPx, ui::kBandHeight) == nullptr) {
    std::fprintf(stderr, "cannot allocate sprites\n");
    return 2;
  }

  std::ofstream states(args.out + "/states.jsonl");
  std::ofstream timeline(args.out + "/outputs.jsonl");
  TraceOutputs sink(timeline);
  core::OutputController outputs(sink);
  outputs.configure(core::OutputSettings{}, time.millis());
  if (!states || !timeline) {
    std::fprintf(stderr, "cannot write to %s\n", args.out.c_str());
    return 2;
  }

  int failures = 0;
  int step = 0;
  core::ViewModel view;
  uint64_t monotonic_ms = 0;
  for (JsonObjectConst entry : scenario["steps"].as<JsonArrayConst>()) {
    const int64_t at = entry["at"] | 0;
    const bool clock_valid = entry["clock_valid"] | true;
    const uint64_t step_ms = std::max(monotonic_ms, static_cast<uint64_t>(at * 1000));
    time.set(clock_valid ? start + at : 0, static_cast<uint32_t>(step_ms));
    sink.begin_step(step, time.millis());
    std::string label = entry["label"] | "";

    if (const char* phase = entry["phase"]) {
      const int status = http_get_status(args.mock + "/_phase/" + phase);
      if (status != 200) {
        std::printf("FAIL step %d: mock server refused phase %s (status %d)\n", step, phase, status);
        ++failures;
      }
      if (label.empty()) {
        label = phase;
      }
    }
    char query[32];
    std::snprintf(query, sizeof(query), "unix=%lld", static_cast<long long>(start + at));
    const int synced = http_get_status(gateway_time_url, query);
    if (synced != 200) {
      std::printf("FAIL step %d: gateway refused the time (status %d)\n", step, synced);
      ++failures;
    }
    const char* network = entry["network"] | "normal";
    const std::string network_query = std::string("profile=") + network;
    if (http_get_status("http://" + args.gateway + "/_network", network_query.c_str()) != 200) {
      std::printf("FAIL step %d: proxy refused network profile %s\n", step, network);
      ++failures;
    }
    if (const char* press = entry["press"]) {
      app.press(std::strcmp(press, "long") == 0 ? core::ButtonEvent::Long : core::ButtonEvent::Short);
      if (label.empty()) {
        label = std::string("press-") + press;
      }
    }
    app.build_view(view);
    outputs.observe(view.time_valid, static_cast<uint16_t>(view.local.hour * 60 + view.local.minute),
                    view.banner, core::Sound::None, time.millis());
    if (JsonObjectConst preview = entry["preview"].as<JsonObjectConst>(); !preview.isNull()) {
      core::OutputProfile draft = outputs.settings().day;
      draft.screen_percent = preview["screen_percent"] | draft.screen_percent;
      draft.led_percent = preview["led_percent"] | draft.led_percent;
      draft.sound_mask = preview["sound_mask"] | draft.sound_mask;
      const char* target = preview["target"] | "screen";
      const char* names[] = {"screen", "led_partial", "led_yellow", "led_red", "led_nodata",
                             "led_clear", "raised", "escalated", "cleared"};
      size_t selected = 0;
      while (selected < sizeof(names) / sizeof(names[0]) && std::strcmp(names[selected], target) != 0) ++selected;
      if (selected == sizeof(names) / sizeof(names[0]) ||
          !outputs.preview(draft, static_cast<core::PreviewTarget>(selected), time.millis())) {
        std::printf("FAIL step %d: preview was refused\n", step);
        ++failures;
      }
    }
    app.refresh_now();
    core::Sound sound = core::Sound::None;
    const uint32_t run_ms = entry["run_ms"] | 0;
    const auto began = std::chrono::steady_clock::now();
    uint32_t elapsed = 0;
    uint32_t max_tick_ms = 0;
    for (;;) {
      elapsed = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - began).count());
      time.set(clock_valid ? start + at : 0, static_cast<uint32_t>(step_ms + elapsed));
      sink.at(time.millis());
      const auto tick_start = std::chrono::steady_clock::now();
      app.tick();
      const auto tick_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - tick_start).count();
      max_tick_ms = std::max(max_tick_ms, static_cast<uint32_t>(tick_ms));
      sink.fetching(app.fetching());
      outputs.gateway_advanced();
      const core::Sound event = app.take_sound();
      if (event != core::Sound::None) sound = event;
      app.build_view(view);
      const uint16_t minute = static_cast<uint16_t>(view.local.hour * 60 + view.local.minute);
      outputs.observe(view.time_valid, minute, view.banner, event, time.millis());
      if (app.settled() && elapsed >= run_ms) break;
      if (elapsed >= 20000) {
        std::printf("FAIL step %d: asynchronous work did not settle within 20 seconds\n", step);
        ++failures;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    monotonic_ms = step_ms + elapsed;
    JsonObjectConst expected = entry["expect"];
    if (expected["screen_percent"].is<int>()) {
      failures += check(step, "screen_percent", std::to_string(expected["screen_percent"].as<int>()),
                        std::to_string(sink.screen));
    }
    if (expected["screen_percent_seen"].is<int>() &&
        sink.screens.count(expected["screen_percent_seen"].as<int>()) == 0) {
      std::printf("FAIL step %d: preview screen percentage was never applied\n", step);
      ++failures;
    }
    if (expected["buzzer_started_min_ms"].is<int>() &&
        (sink.first_buzzer_ms == UINT32_MAX || sink.first_buzzer_ms < expected["buzzer_started_min_ms"].as<unsigned>())) {
      std::printf("FAIL step %d: buzzer first started at %u ms\n", step, sink.first_buzzer_ms);
      ++failures;
    }
    if (expected["buzzer_started_max_ms"].is<int>() &&
        sink.first_buzzer_ms > expected["buzzer_started_max_ms"].as<unsigned>()) {
      std::printf("FAIL step %d: queued buzzer started too late at %u ms\n", step, sink.first_buzzer_ms);
      ++failures;
    }
    if (expected["preview_busy"].is<bool>()) {
      failures += check(step, "preview_busy", expected["preview_busy"].as<bool>() ? "yes" : "no",
                        outputs.preview_busy() ? "yes" : "no");
    }
    if (expected["night"].is<bool>()) {
      failures += check(step, "night", expected["night"].as<bool>() ? "yes" : "no", outputs.night() ? "yes" : "no");
    }
    if (expected["buzzer_hz_seen"].is<int>() &&
        sink.frequencies.count(expected["buzzer_hz_seen"].as<int>()) == 0) {
      std::printf("FAIL step %d: buzzer never reached %d Hz\n", step, expected["buzzer_hz_seen"].as<int>());
      ++failures;
    }
    if (expected["buzzer_hz_absent"].is<int>() &&
        sink.frequencies.count(expected["buzzer_hz_absent"].as<int>()) != 0) {
      std::printf("FAIL step %d: muted buzzer reached %d Hz\n", step, expected["buzzer_hz_absent"].as<int>());
      ++failures;
    }
    if (expected["buzzer_off"].is<bool>()) {
      failures += check(step, "buzzer_off", expected["buzzer_off"].as<bool>() ? "yes" : "no",
                        sink.frequency == 0 ? "yes" : "no");
    }
    if (expected["led_changes_min"].is<int>() && sink.led_changes < expected["led_changes_min"].as<int>()) {
      std::printf("FAIL step %d: LED changed only %d times\n", step, sink.led_changes);
      ++failures;
    }
    if (expected["led_changes_fetching_min"].is<int>() &&
        sink.led_changes_fetching < expected["led_changes_fetching_min"].as<int>()) {
      std::printf("FAIL step %d: LED changed only %d times during gateway work\n", step, sink.led_changes_fetching);
      ++failures;
    }
    if (expected["tick_ms_max"].is<int>() && max_tick_ms > expected["tick_ms_max"].as<unsigned>()) {
      std::printf("FAIL step %d: app tick blocked for %u ms\n", step, max_tick_ms);
      ++failures;
    }
    if (expected["outcome"].is<const char*>()) {
      failures += check(step, "outcome", expected["outcome"].as<const char*>(),
                        core::fetch_outcome_name(app.status().last_outcome));
    }
    app.build_view(view);
    view.notice = entry["notice"] | false;
    ui::render(canvas, band, view);

    char name[16];
    std::snprintf(name, sizeof(name), "%02d-", step);
    const std::string png = args.out + "/" + name + (label.empty() ? "step" : label) + ".png";
    if (!write_png(canvas, png)) {
      std::printf("FAIL step %d: cannot write %s\n", step, png.c_str());
      ++failures;
    }
    write_state(states, step, at, label, view, sound, app);
    const int step_failures = check_expectations(step, entry["expect"], view, sound, app);
    if (step_failures == 0) {
      std::printf("ok   step %d %-14s panel=%s banner=%s markers=%u sound=%s outcome=%s\n", step, label.c_str(),
                  core::panel_name(view.panel), core::banner_name(view.banner),
                  static_cast<unsigned>(app.snapshot().markers.size()), core::sound_name(sound),
                  core::fetch_outcome_name(app.status().last_outcome));
    }
    failures += step_failures;
    ++step;
  }
  std::printf("%s: %d step(s), %d failure(s)\n", args.scenario.c_str(), step, failures);
  return failures == 0 ? 0 : 1;
}
