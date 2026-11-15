#include "output_policy.h"

namespace core {
namespace {
uint32_t colour(Banner banner) {
  switch (banner) {
    case Banner::Red: return 0x400000;
    case Banner::Yellow: return 0x302000;
    case Banner::Partial: return 0x200000;
    case Banner::NoData: return 0x180800;
    case Banner::None: return 0;
  }
  return 0;
}
uint32_t scale(uint32_t rgb, uint32_t numerator, uint32_t denominator) {
  return (((rgb >> 16 & 255) * numerator / denominator) << 16) |
         (((rgb >> 8 & 255) * numerator / denominator) << 8) |
         ((rgb & 255) * numerator / denominator);
}
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
  return p.screen_percent <= 100 && p.led_percent <= 100 && p.sound_mask <= 7;
}
bool valid_output_settings(const OutputSettings& s) {
  return valid_output_profile(s.day) && valid_output_profile(s.night) &&
         s.night_start < 1440 && s.night_end < 1440 && s.night_start != s.night_end;
}
bool OutputController::allowed(Sound sound) const {
  return sound != Sound::None && (profile().sound_mask & (1U << (static_cast<unsigned>(sound) - 1))) != 0;
}
bool OutputController::compatible(Sound sound) const {
  switch (sound) {
    case Sound::AlertRaised:
      return banner_ == Banner::Partial || banner_ == Banner::Yellow || banner_ == Banner::Red;
    case Sound::AlertEscalated: return banner_ == Banner::Red;
    case Sound::AlertCleared: return banner_ == Banner::None;
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
    if (banner == Banner::None) {
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
  if (preview_active_ || preview_yield_ || sound_busy() || !valid_output_profile(p) ||
      static_cast<unsigned>(target) > static_cast<unsigned>(PreviewTarget::Cleared)) return false;
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
    const uint32_t limit = selected == Sound::None ? 3000 : duration(selected);
    if (now - preview_at_ >= limit) {
      preview_active_ = false;
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
  if (preview_active_) {
    const uint32_t elapsed = now - preview_at_;
    switch (preview_target_) {
      case PreviewTarget::Screen: screen = preview_profile_.screen_percent; break;
      case PreviewTarget::LedPartial: rgb = elapsed < 1800 ? pulse(colour(Banner::Partial), elapsed) : colour(Banner::Partial); break;
      case PreviewTarget::LedYellow: rgb = elapsed < 1800 ? pulse(colour(Banner::Yellow), elapsed) : colour(Banner::Yellow); break;
      case PreviewTarget::LedRed: rgb = elapsed < 1800 ? pulse(colour(Banner::Red), elapsed) : colour(Banner::Red); break;
      case PreviewTarget::LedNoData: rgb = no_data(elapsed); break;
      case PreviewTarget::LedClear: rgb = elapsed < 400 ? scale(colour(Banner::Red), 400 - elapsed, 400) : 0; break;
      default: break;
    }
    if (preview_target_ >= PreviewTarget::LedPartial && preview_target_ <= PreviewTarget::LedClear)
      rgb = scale(rgb, preview_profile_.led_percent, 100);
  }
  const uint16_t frequency = note(sound_, now - sound_at_);
  if (screen != screen_) { screen_ = screen; sink_.backlight(screen); }
  if (frequency != frequency_) { frequency_ = frequency; sink_.buzzer(frequency); }
  if (rgb != rgb_) {
    rgb_ = rgb;
    sink_.led(static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
  }
}
static_assert(sizeof(OutputProfile) == 3, "profiles must remain compact");
static_assert(sizeof(OutputController) <= 96, "output state exceeds RAM budget");
}  // namespace core
