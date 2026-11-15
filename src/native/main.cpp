// Scenario runner: drives the clock's App against the mock server with a scripted clock
// and button, renders every step through the same UI code as the device, and checks
// each step's expectations.
//
//   program --scenario fixtures/scenarios/x.json --base http://127.0.0.1:PORT --out DIR
//
// Writes DIR/NN-<label>.png and DIR/states.jsonl. Exit status: 0 when every expectation
// held, 1 when one failed, 2 on bad input.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "gfx.h"

#include "app.h"
#include "host_platform.h"
#include "json.h"
#include "scene.h"
#include "time_util.h"

namespace {

constexpr const char* kTimeZone = "EET-2EEST,M3.5.0/3,M10.5.0/4";  // Europe/Kyiv
constexpr const char* kTestToken = "test-token";
constexpr int kTicksPerStep = 4;  // alerts, threats, and slack for retries

struct Args {
  std::string scenario;
  std::string base;
  std::string out;
};

bool parse_args(int argc, char** argv, Args& args) {
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--scenario") {
      args.scenario = argv[i + 1];
    } else if (flag == "--base") {
      args.base = argv[i + 1];
    } else if (flag == "--out") {
      args.out = argv[i + 1];
    } else {
      return false;
    }
  }
  return !args.scenario.empty() && !args.base.empty() && !args.out.empty();
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
  if (const char* parse = expect["threats_parse"]) {
    failures += check(step, "threats_parse", parse, core::threat_parse_name(app.last_threat_parse()));
  }
  if (expect["markers_min"].is<int>() && static_cast<int>(view.markers.size()) < expect["markers_min"].as<int>()) {
    std::printf("FAIL step %d: markers expected >= %d, got %u\n", step, expect["markers_min"].as<int>(),
                static_cast<unsigned>(view.markers.size()));
    ++failures;
  }
  if (expect["markers_max"].is<int>() && static_cast<int>(view.markers.size()) > expect["markers_max"].as<int>()) {
    std::printf("FAIL step %d: markers expected <= %d, got %u\n", step, expect["markers_max"].as<int>(),
                static_cast<unsigned>(view.markers.size()));
    ++failures;
  }
  return failures;
}

void write_state(std::ofstream& out, int step, int64_t at, const std::string& label,
                 const core::ViewModel& view, core::Sound sound, const core::App& app) {
  JsonDocument line;
  line["step"] = step;
  line["at"] = static_cast<long>(at);
  line["label"] = label;
  line["panel"] = core::panel_name(view.panel);
  line["banner"] = core::banner_name(view.banner);
  line["markers"] = view.markers.size();
  line["threats_in_watch"] = view.threats_in_watch;
  line["sound"] = core::sound_name(sound);
  line["alerts_outcome"] = core::fetch_outcome_name(app.alerts_status().last_outcome);
  line["threats_outcome"] = core::fetch_outcome_name(app.threats_status().last_outcome);
  line["threats_parse"] = core::threat_parse_name(app.last_threat_parse());
  line["threats_total"] = app.threats().total;
  line["arena_high_water"] = core::threat_parse_stats().arena_high_water;
  line["filter_bytes"] = core::threat_parse_stats().filter_bytes;
  std::string text;
  serializeJson(line, text);
  out << text << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    std::fprintf(stderr, "usage: %s --scenario FILE --base URL --out DIR\n", argv[0]);
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

  // These strings must outlive the App, which keeps pointers to them.
  const std::string threats_url = args.base + "/api/v1/threats";
  const std::string alerts_url = args.base + "/v2/iot/active_air_raid_alerts/31.json";
  const std::string home_region = scenario["home_region"] | "";

  core::Settings settings;
  settings.home = core::LatLon{scenario["home"][0] | 50.4501f, scenario["home"][1] | 30.5234f};
  settings.home_region = home_region.empty() ? nullptr : home_region.c_str();
  settings.map_radius_km = scenario["map_radius_km"] | settings.map_radius_km;
  settings.watch_radius_km = scenario["watch_radius_km"] | settings.watch_radius_km;
  settings.threats_url = threats_url.c_str();
  settings.alerts_url = alerts_url.c_str();
  settings.alerts_token = kTestToken;

  FakeTime time;
  SocketTransport transport;
  core::App app(settings, time, transport);

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
  if (!states) {
    std::fprintf(stderr, "cannot write to %s\n", args.out.c_str());
    return 2;
  }

  int failures = 0;
  int step = 0;
  core::ViewModel view;
  for (JsonObjectConst entry : scenario["steps"].as<JsonArrayConst>()) {
    const int64_t at = entry["at"] | 0;
    time.set(start + at, static_cast<uint32_t>(at * 1000));
    std::string label = entry["label"] | "";

    if (const char* phase = entry["phase"]) {
      const int status = http_get_status(args.base + "/_phase/" + phase);
      if (status != 200) {
        std::printf("FAIL step %d: mock server refused phase %s (status %d)\n", step, phase, status);
        ++failures;
      }
      app.refresh_now();
      if (label.empty()) {
        label = phase;
      }
    }
    if (const char* press = entry["press"]) {
      app.press(std::strcmp(press, "long") == 0 ? core::ButtonEvent::Long : core::ButtonEvent::Short);
      if (label.empty()) {
        label = std::string("press-") + press;
      }
    }
    for (int i = 0; i < kTicksPerStep; ++i) {
      app.tick();
    }
    const core::Sound sound = app.take_sound();
    app.build_view(view);
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
      std::printf("ok   step %d %-14s panel=%s banner=%s markers=%u sound=%s\n", step, label.c_str(),
                  core::panel_name(view.panel), core::banner_name(view.banner),
                  static_cast<unsigned>(view.markers.size()), core::sound_name(sound));
    }
    failures += step_failures;
    ++step;
  }
  std::printf("%s: %d step(s), %d failure(s)\n", args.scenario.c_str(), step, failures);
  return failures == 0 ? 0 : 1;
}
