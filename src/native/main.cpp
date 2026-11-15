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
constexpr int kTicksPerStep = 3;  // snapshot, a changed basemap, and one spare

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
  const int markers = static_cast<int>(app.snapshot().markers.size());
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
    if (const char* press = entry["press"]) {
      app.press(std::strcmp(press, "long") == 0 ? core::ButtonEvent::Long : core::ButtonEvent::Short);
      if (label.empty()) {
        label = std::string("press-") + press;
      }
    }
    app.refresh_now();
    for (int i = 0; i < kTicksPerStep; ++i) {
      app.tick();
    }
    const core::Sound sound = app.take_sound();
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
