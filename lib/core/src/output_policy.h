#pragma once

#include "app.h"

namespace core {

struct OutputProfile {
  uint8_t screen_percent = 80;
  uint8_t led_percent = 100;
  uint8_t sound_mask = 7;
  uint8_t volume_percent = 100;  // 1-100; sets the buzzer duty cycle
};
struct OutputSettings {
  OutputProfile day;
  OutputProfile night{5, 5, 0, 100};
  uint16_t night_start = 23 * 60;
  uint16_t night_end = 10 * 60;
  bool sound_on = true;  // off silences automatic sounds in both profiles; previews still play
};
bool valid_output_profile(const OutputProfile& profile);
bool valid_output_settings(const OutputSettings& settings);
// The buzzer's high time per period, in thousandths, that gives `volume_percent` of full loudness:
// the fundamental of a square wave with duty d has amplitude sin(pi * d), so 500 is full volume.
uint16_t buzzer_duty_permille(uint8_t volume_percent);

enum class PreviewTarget : uint8_t {
  Screen, LedYellow, LedRed, LedNoData, LedClear, Raised, Escalated, Cleared
};
class OutputSink {
 public:
  virtual void backlight(uint8_t percent) = 0;
  virtual void buzzer(uint16_t frequency_hz, uint8_t volume_percent) = 0;
  virtual void led(uint8_t red, uint8_t green, uint8_t blue) = 0;
 protected:
  ~OutputSink() = default;
};
class OutputController {
 public:
  explicit OutputController(OutputSink& sink) : sink_(sink) {}
  void configure(const OutputSettings& settings, uint32_t now);
  void observe(bool time_valid, uint16_t local_minute, Banner banner, Sound event, uint32_t now);
  void update(uint32_t now);
  bool preview(const OutputProfile& profile, PreviewTarget target, uint32_t now);
  void gateway_advanced() { preview_yield_ = false; }
  const OutputSettings& settings() const { return settings_; }
  bool night() const { return night_; }
  bool preview_busy() const { return preview_active_; }
  bool sound_busy() const { return sound_ != Sound::None; }
  Sound active_sound() const { return sound_; }
  uint8_t screen_percent() const { return screen_; }
  uint16_t frequency_hz() const { return frequency_; }
  uint8_t volume_percent() const { return volume_; }
  uint32_t led_rgb() const { return rgb_; }
 private:
  const OutputProfile& profile() const { return night_ ? settings_.night : settings_.day; }
  bool allowed(Sound sound) const;
  bool compatible(Sound sound) const;
  void start_sound(Sound sound, uint32_t now);
  void enforce_policy();
  void paint(uint32_t now);
  uint32_t live_rgb(uint32_t now) const;
  OutputSink& sink_;
  OutputSettings settings_;
  OutputProfile preview_profile_;
  uint32_t sound_at_ = 0;
  uint32_t led_at_ = 0;
  uint32_t preview_at_ = 0;
  uint32_t fade_at_ = 0;   // start of the LED fade into or out of a preview
  uint32_t fade_rgb_ = 0;
  uint32_t from_rgb_ = 0;  // shown when an LED preview started or ended; faded out of
  uint32_t rgb_ = UINT32_MAX;
  uint16_t frequency_ = UINT16_MAX;
  uint16_t minute_ = 0;
  uint8_t screen_ = UINT8_MAX;
  uint8_t volume_ = UINT8_MAX;
  Banner banner_ = Banner::NoData;
  Sound sound_ = Sound::None;
  Sound pending_ = Sound::None;
  PreviewTarget preview_target_ = PreviewTarget::Screen;
  enum class Animation : uint8_t { Steady, Pulses, Clear } animation_ = Animation::Steady;
  bool time_valid_ = false;
  bool night_ = true;
  bool preview_active_ = false;
  bool preview_yield_ = false;
  bool returning_ = false;  // fading from the last preview colour back to the live one
};

}  // namespace core
