#include <unity.h>
#include "output_policy.h"
using namespace core;
struct Sink final : OutputSink {
  uint8_t screen = 0;
  uint16_t hz = 0;
  uint32_t rgb = 0;
  void backlight(uint8_t value) override { screen = value; }
  void buzzer(uint16_t value) override { hz = value; }
  void led(uint8_t r, uint8_t g, uint8_t b) override { rgb = uint32_t(r)<<16 | uint32_t(g)<<8 | b; }
};
void setUp() {}
void tearDown() {}
void test_profiles_apply_at_boundaries_and_stop_automatic_sound() {
  Sink sink; OutputController out(sink); OutputSettings settings;
  out.configure(settings, 0);
  TEST_ASSERT_EQUAL_UINT8(5, sink.screen);
  out.observe(true, 600, Banner::Red, Sound::AlertRaised, 1);
  TEST_ASSERT_EQUAL_UINT8(80, sink.screen);
  TEST_ASSERT_EQUAL_UINT16(2730, sink.hz);
  out.observe(true, 1380, Banner::Red, Sound::None, 2);
  TEST_ASSERT_EQUAL_UINT8(5, sink.screen);
  TEST_ASSERT_EQUAL_UINT16(0, sink.hz);
  out.observe(true, 599, Banner::Red, Sound::None, 3);
  TEST_ASSERT_TRUE(out.night());
  out.observe(false, 900, Banner::Red, Sound::None, 4);
  TEST_ASSERT_TRUE(out.night());
  settings.night_start = 600; settings.night_end = 660;
  out.configure(settings, 5);
  out.observe(true, 600, Banner::Red, Sound::None, 6);
  TEST_ASSERT_TRUE(out.night());
  out.observe(true, 660, Banner::Red, Sound::None, 7);
  TEST_ASSERT_FALSE(out.night());
}
void test_preview_finishes_and_replays_only_latest_permitted_live_event() {
  Sink sink; OutputController out(sink);
  out.observe(false, 0, Banner::None, Sound::None, 0);
  TEST_ASSERT_TRUE(out.preview(OutputProfile{}, PreviewTarget::Raised, 10));
  TEST_ASSERT_EQUAL_UINT16(2730, sink.hz);
  TEST_ASSERT_FALSE(out.preview(OutputProfile{}, PreviewTarget::Screen, 11));
  out.configure(OutputSettings{}, 15);
  TEST_ASSERT_EQUAL_UINT16(2730, sink.hz);
  out.observe(false, 0, Banner::Red, Sound::AlertRaised, 20);
  out.update(1310);
  TEST_ASSERT_FALSE(out.sound_busy());
  TEST_ASSERT_EQUAL_UINT16(0, sink.hz);
  TEST_ASSERT_FALSE(out.preview(OutputProfile{}, PreviewTarget::Screen, 1311));
  out.gateway_advanced();
  out.observe(true, 720, Banner::Red, Sound::None, 1312);
  TEST_ASSERT_TRUE(out.preview(OutputProfile{}, PreviewTarget::Screen, 1400));
  out.observe(true, 720, Banner::Red, Sound::AlertEscalated, 1500);
  out.observe(true, 720, Banner::Yellow, Sound::None, 1600);
  out.update(4400);
  TEST_ASSERT_FALSE(out.sound_busy());
  TEST_ASSERT_EQUAL_HEX32(0x302000, sink.rgb);
  out.gateway_advanced();
  TEST_ASSERT_TRUE(out.preview(OutputProfile{}, PreviewTarget::Screen, 4500));
  out.observe(true, 720, Banner::Red, Sound::AlertRaised, 4600);
  out.observe(true, 720, Banner::None, Sound::AlertCleared, 4700);
  out.update(7500);
  TEST_ASSERT_EQUAL_INT(int(Sound::AlertCleared), int(out.active_sound()));
  TEST_ASSERT_EQUAL_UINT16(1400, sink.hz);
  out.update(7580);
  TEST_ASSERT_FALSE(out.sound_busy());
}
void test_muted_new_event_and_settings_save_do_not_resurrect_old_alerts() {
  Sink sink; OutputController out(sink); OutputSettings settings;
  out.observe(true, 720, Banner::None, Sound::None, 0);
  TEST_ASSERT_TRUE(out.preview(OutputProfile{19, 37, 0}, PreviewTarget::Screen, 1));
  TEST_ASSERT_EQUAL_UINT8(19, sink.screen);
  out.observe(true, 720, Banner::Red, Sound::AlertRaised, 2);
  settings.day.sound_mask = 1;
  settings.day.screen_percent = 44;
  out.configure(settings, 3);
  TEST_ASSERT_EQUAL_UINT8(19, sink.screen);
  out.observe(true, 720, Banner::Red, Sound::AlertEscalated, 4);
  out.update(3001);
  TEST_ASSERT_EQUAL_UINT8(44, sink.screen);
  TEST_ASSERT_FALSE(out.sound_busy());
  out.gateway_advanced();
  TEST_ASSERT_TRUE(out.preview(OutputProfile{}, PreviewTarget::Screen, 4000));
  out.observe(true, 720, Banner::Red, Sound::AlertRaised, 4001);
  out.observe(true, 720, Banner::NoData, Sound::None, 4002);
  out.update(7000);
  TEST_ASSERT_FALSE(out.sound_busy());
}
void test_animations_and_sound_deadlines_survive_wrap_and_late_updates() {
  Sink sink; OutputController out(sink);
  const uint32_t start = UINT32_MAX - 200;
  out.observe(true, 720, Banner::Red, Sound::AlertRaised, start);
  out.update(start + 250);
  TEST_ASSERT_EQUAL_HEX32(0x400000, sink.rgb);
  out.update(start + 500);
  TEST_ASSERT_EQUAL_HEX32(0, sink.rgb);
  out.update(start + 1800);
  TEST_ASSERT_EQUAL_HEX32(0x400000, sink.rgb);
  TEST_ASSERT_EQUAL_UINT16(0, sink.hz);
  out.observe(true, 720, Banner::None, Sound::AlertCleared, start + 1801);
  TEST_ASSERT_EQUAL_UINT16(1400, sink.hz);
  out.update(start + 1881);
  TEST_ASSERT_EQUAL_UINT16(0, sink.hz);
  out.update(start + 2001);
  TEST_ASSERT_EQUAL_HEX32(0x200000, sink.rgb);
  out.update(start + 2201);
  TEST_ASSERT_EQUAL_HEX32(0, sink.rgb);
  out.observe(true, 720, Banner::NoData, Sound::None, 3000);
  out.update(3100); TEST_ASSERT_EQUAL_HEX32(0x180800, sink.rgb);
  out.update(3200); TEST_ASSERT_EQUAL_HEX32(0, sink.rgb);
  out.update(3450); TEST_ASSERT_EQUAL_HEX32(0x180800, sink.rgb);
  out.update(3550); TEST_ASSERT_EQUAL_HEX32(0, sink.rgb);
  out.update(8100); TEST_ASSERT_EQUAL_HEX32(0x180800, sink.rgb);
}
void test_automatic_sound_stops_when_live_state_invalidates_it() {
  for (int kind = 0; kind < 3; ++kind) {
    Sink sink; OutputController out(sink); OutputSettings settings;
    if (kind == 2) settings.day.sound_mask = 1;
    out.configure(settings, 0);
    out.observe(true, 720, Banner::Red,
                kind == 0 ? Sound::AlertEscalated : Sound::AlertRaised, 100);
    TEST_ASSERT_EQUAL_UINT16(2730, sink.hz);
    out.observe(true, 720, kind == 0 ? Banner::Yellow : kind == 1 ? Banner::NoData : Banner::None,
                kind == 2 ? Sound::AlertCleared : Sound::None, 110);
    TEST_ASSERT_EQUAL_UINT16(0, sink.hz);
    settings.day.sound_mask = 7;
    out.configure(settings, 120);
    TEST_ASSERT_FALSE(out.sound_busy());
  }
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_automatic_sound_stops_when_live_state_invalidates_it);
  RUN_TEST(test_profiles_apply_at_boundaries_and_stop_automatic_sound);
  RUN_TEST(test_preview_finishes_and_replays_only_latest_permitted_live_event);
  RUN_TEST(test_muted_new_event_and_settings_save_do_not_resurrect_old_alerts);
  RUN_TEST(test_animations_and_sound_deadlines_survive_wrap_and_late_updates);
  return UNITY_END();
}
