#include "output_policy.h"

namespace core {
namespace {
uint32_t colour(Banner banner) {
  switch (banner) {
    case Banner::Red: return 0x400000;
    case Banner::Yellow: return 0x302000;
    case Banner::NoData: return 0x180800;
    case Banner::Partial:  // alerts elsewhere in the oblast keep the LED dark
    case Banner::None: return 0;
  }
  return 0;
}
uint32_t scale(uint32_t rgb, uint32_t numerator, uint32_t denominator) {
  return (((rgb >> 16 & 255) * numerator / denominator) << 16) |
         (((rgb >> 8 & 255) * numerator / denominator) << 8) |
         ((rgb & 255) * numerator / denominator);
}
// Blends from `from` towards `to`; n/d is the share of `to`.
uint32_t mix(uint32_t from, uint32_t to, uint32_t n, uint32_t d) {
  uint32_t out = 0;
  for (uint32_t shift = 0; shift <= 16; shift += 8) {
    const int32_t a = static_cast<int32_t>(from >> shift & 255);
    const int32_t b = static_cast<int32_t>(to >> shift & 255);
    out |= static_cast<uint32_t>(a + (b - a) * static_cast<int32_t>(n) / static_cast<int32_t>(d)) << shift;
  }
  return out;
}
constexpr uint32_t kFadeMs = 300;     // into a preview's lead-in and back to the live colour
constexpr uint32_t kLeadMs = 1000;    // an LED preview first holds the colour its pattern starts from
constexpr uint32_t kPatternMs = 2500;
constexpr uint32_t kScreenPreviewMs = 3000;
uint32_t pulse(uint32_t rgb, uint32_t elapsed) {
  const uint32_t phase = elapsed % 600;
  if (phase < 250) return scale(rgb, phase, 250);
  if (phase < 500) return scale(rgb, 500 - phase, 250);
  return 0;
}
uint32_t no_data(uint32_t elapsed) {
  uint32_t phase = elapsed % 5000;
  if (phase >= 350 && phase < 550) phase -= 350;
  else if (phase >= 200) return 0;
  return scale(colour(Banner::NoData), phase < 100 ? phase : 200 - phase, 100);
}
uint32_t duration(Sound sound) {
  switch (sound) {
    case Sound::AlertRaised: return 1300;
    case Sound::AlertEscalated: return 1150;
    case Sound::AlertCleared: return 80;
    case Sound::None: return 0;
  }
  return 0;
}
uint16_t note(Sound sound, uint32_t elapsed) {
  if (elapsed >= duration(sound)) return 0;
  if (sound == Sound::AlertCleared) return 1400;
  const uint32_t on = sound == Sound::AlertRaised ? 300 : 150;
  const uint32_t off = sound == Sound::AlertRaised ? 200 : 100;
  return elapsed % (on + off) < on ? 2730 : 0;
}
bool led_target(PreviewTarget target) {
  return target >= PreviewTarget::LedYellow && target <= PreviewTarget::LedClear;
}
// The LED pattern of a preview, `elapsed` after its lead-in; at 0 it is the lead-in colour.
uint32_t preview_rgb(PreviewTarget target, uint32_t elapsed) {
  switch (target) {
    case PreviewTarget::LedYellow:
      return elapsed < 1800 ? pulse(colour(Banner::Yellow), elapsed) : colour(Banner::Yellow);
    case PreviewTarget::LedRed:
      return elapsed < 1800 ? pulse(colour(Banner::Red), elapsed) : colour(Banner::Red);
    case PreviewTarget::LedNoData: return no_data(elapsed);
    case PreviewTarget::LedClear: return elapsed < 400 ? scale(colour(Banner::Red), 400 - elapsed, 400) : 0;
    default: return 0;
  }
}
Sound preview_sound(PreviewTarget target) {
  switch (target) {
    case PreviewTarget::Raised: return Sound::AlertRaised;
    case PreviewTarget::Escalated: return Sound::AlertEscalated;
    case PreviewTarget::Cleared: return Sound::AlertCleared;
    default: return Sound::None;
  }
}
}
bool valid_output_profile(const OutputProfile& p) {
  return p.screen_percent <= 100 && p.led_percent <= 100 && p.sound_mask <= 7 &&
         p.volume_percent >= 1 && p.volume_percent <= 100;
}
uint16_t buzzer_duty_permille(uint8_t volume) {
  // asin(v) / pi in two-thousandths at v = 0, 0.05, ..., 1 (bytes save RAM), interpolated.
  static constexpr uint8_t kTable[] = {0, 8, 16, 24, 32, 40, 48, 57, 65, 74, 83,
                                       93, 102, 113, 123, 135, 148, 162, 178, 199, 250};
  if (volume >= 100) return 2U * kTable[20];
  const unsigned index = volume / 5U;
  const unsigned rest = volume % 5U;
  return static_cast<uint16_t>(2U * kTable[index] + 2U * (kTable[index + 1] - kTable[index]) * rest / 5U);
}
bool valid_output_settings(const OutputSettings& s) {
  return valid_output_profile(s.day) && valid_output_profile(s.night) &&
         s.night_start < 1440 && s.night_end < 1440 && s.night_start != s.night_end;
}
bool OutputController::allowed(Sound sound) const {
  return sound != Sound::None && settings_.sound_on && (profile().sound_mask & (1U << (static_cast<unsigned>(sound) - 1))) != 0;
}
bool OutputController::compatible(Sound sound) const {
  switch (sound) {
    case Sound::AlertRaised:
      return banner_ == Banner::Yellow || banner_ == Banner::Red;
    case Sound::AlertEscalated: return banner_ == Banner::Red;
    case Sound::AlertCleared: return banner_ == Banner::None || banner_ == Banner::Partial;
    case Sound::None: return false;
  }
  return false;
}
void OutputController::enforce_policy() {
  night_ = !time_valid_ || (settings_.night_start < settings_.night_end
      ? minute_ >= settings_.night_start && minute_ < settings_.night_end
      : minute_ >= settings_.night_start || minute_ < settings_.night_end);
  if (!allowed(pending_) || !compatible(pending_)) pending_ = Sound::None;
  if (!preview_active_ && (!allowed(sound_) || !compatible(sound_))) sound_ = Sound::None;
}
void OutputController::configure(const OutputSettings& settings, uint32_t now) {
  if (!valid_output_settings(settings)) return;
  settings_ = settings;
  enforce_policy();
  update(now);
}
void OutputController::start_sound(Sound sound, uint32_t now) {
  sound_ = sound;
  sound_at_ = now;
}
void OutputController::observe(bool valid, uint16_t minute, Banner banner, Sound event, uint32_t now) {
  time_valid_ = valid && minute < 1440;
  minute_ = minute;
  if (banner != banner_) {
    if (colour(banner) == 0) {
      fade_rgb_ = live_rgb(now);
      animation_ = Animation::Clear;
    } else animation_ = Animation::Steady;
    banner_ = banner;
    led_at_ = now;
  }
  enforce_policy();
  if (event != Sound::None) {
    if ((event == Sound::AlertRaised || event == Sound::AlertEscalated) && compatible(event)) {
      animation_ = Animation::Pulses;
      led_at_ = now;
    }
    pending_ = allowed(event) && compatible(event) ? event : Sound::None;
    if (!preview_active_) {
      if (pending_ != Sound::None) start_sound(pending_, now);
      pending_ = Sound::None;
    }
  }
  update(now);
}
uint32_t OutputController::live_rgb(uint32_t now) const {
  const uint32_t elapsed = now - led_at_;
  if (banner_ == Banner::NoData) return no_data(elapsed);
  if (animation_ == Animation::Clear && elapsed < 400) return scale(fade_rgb_, 400 - elapsed, 400);
  uint32_t rgb = colour(banner_);
  if (animation_ == Animation::Pulses && elapsed < 1800) rgb = pulse(rgb, elapsed);
  return rgb;
}
bool OutputController::preview(const OutputProfile& p, PreviewTarget target, uint32_t now) {
  // A new preview replaces a running one; a live sound or a just-finished preview's pending
  // sound comes first.
  if (preview_yield_ || (sound_busy() && !preview_active_) || !valid_output_profile(p) ||
      static_cast<unsigned>(target) > static_cast<unsigned>(PreviewTarget::Cleared)) return false;
  // An LED preview replaced by a screen or sound preview fades back to the live colour.
  returning_ = preview_active_ && led_target(preview_target_) && !led_target(target);
  from_rgb_ = rgb_ == UINT32_MAX ? 0 : rgb_;
  fade_at_ = now;
  preview_profile_ = p;
  preview_target_ = target;
  preview_active_ = true;
  preview_at_ = now;
  start_sound(preview_sound(target), now);
  update(now);
  return true;
}
void OutputController::update(uint32_t now) {
  if (preview_active_) {
    const Sound selected = preview_sound(preview_target_);
    const uint32_t limit = selected != Sound::None ? duration(selected)
        : led_target(preview_target_) ? kLeadMs + kPatternMs : kScreenPreviewMs;
    if (now - preview_at_ >= limit) {
      preview_active_ = false;
      returning_ = led_target(preview_target_);
      from_rgb_ = rgb_;
      fade_at_ = now;
      preview_yield_ = true;
      sound_ = Sound::None;
      enforce_policy();
      if (pending_ != Sound::None) start_sound(pending_, now);
      pending_ = Sound::None;
    }
  }
  if (sound_ != Sound::None && now - sound_at_ >= duration(sound_)) sound_ = Sound::None;
  paint(now);
}
void OutputController::paint(uint32_t now) {
  uint8_t screen = profile().screen_percent;
  uint32_t rgb = scale(live_rgb(now), profile().led_percent, 100);
  const uint32_t faded = now - fade_at_;
  if (preview_active_ && preview_target_ == PreviewTarget::Screen) screen = preview_profile_.screen_percent;
  if (preview_active_ && led_target(preview_target_)) {
    const uint32_t elapsed = now - preview_at_;
    rgb = scale(preview_rgb(preview_target_, elapsed < kLeadMs ? 0 : elapsed - kLeadMs),
                preview_profile_.led_percent, 100);
    if (faded < kFadeMs) rgb = mix(from_rgb_, rgb, faded, kFadeMs);
  } else if (returning_) {
    if (faded < kFadeMs) rgb = mix(from_rgb_, rgb, faded, kFadeMs);
    else returning_ = false;
  }
  const uint16_t frequency = note(sound_, now - sound_at_);
  // A sound preview plays at the draft volume; a live sound that waited for it plays at the live one.
  const uint8_t volume = preview_active_ && preview_sound(preview_target_) != Sound::None
      ? preview_profile_.volume_percent : profile().volume_percent;
  if (screen != screen_) { screen_ = screen; sink_.backlight(screen); }
  if (frequency != frequency_ || (frequency != 0 && volume != volume_)) {
    frequency_ = frequency;
    volume_ = volume;
    sink_.buzzer(frequency, volume);
  }
  if (rgb != rgb_) {
    rgb_ = rgb;
    sink_.led(static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
  }
}
static_assert(sizeof(OutputProfile) == 4, "profiles must remain compact");
static_assert(sizeof(OutputController) <= 96, "output state exceeds RAM budget");
}  // namespace core
