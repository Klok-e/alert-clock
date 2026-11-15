#include "outputs.h"

#include <Arduino.h>

namespace {

using Note = Outputs::Note;

// 2730 Hz is the supplier firmware's alarm tone; 1400/1000 Hz its key tones.
constexpr Note kRaised[] = {{2730, 300}, {0, 200}, {2730, 300}, {0, 200}, {2730, 300}};
constexpr Note kEscalated[] = {{2730, 150}, {0, 100}, {2730, 150}, {0, 100}, {2730, 150},
                               {0, 100},    {2730, 150}, {0, 100}, {2730, 150}};
constexpr Note kCleared[] = {{1400, 150}, {0, 100}, {1000, 300}};

constexpr uint16_t kPwmRange = 1023;

}  // namespace

void Outputs::begin() {
  pinMode(board::kBuzzerPin, OUTPUT);
  digitalWrite(board::kBuzzerPin, LOW);
  pinMode(board::kButtonPin, INPUT);
  pinMode(board::kBacklightPin, OUTPUT);
  analogWriteRange(kPwmRange);  // core 3.x defaults to 255; the math below uses 1023
  set_backlight(0);
  led_.begin();
  led_.clear();
  led_.show();
}

void Outputs::set_backlight(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  analogWrite(board::kBacklightPin, kPwmRange - (kPwmRange * percent) / 100);
}

void Outputs::play(core::Sound sound) {
  switch (sound) {
    case core::Sound::AlertRaised:
      pattern_ = kRaised;
      pattern_length_ = sizeof(kRaised) / sizeof(kRaised[0]);
      break;
    case core::Sound::AlertEscalated:
      pattern_ = kEscalated;
      pattern_length_ = sizeof(kEscalated) / sizeof(kEscalated[0]);
      break;
    case core::Sound::AlertCleared:
      pattern_ = kCleared;
      pattern_length_ = sizeof(kCleared) / sizeof(kCleared[0]);
      break;
    case core::Sound::None:
      return;
  }
  index_ = 0;
  pending_start_ = true;
}

void Outputs::start_note(uint32_t now_ms) {
  note_started_ms_ = now_ms;
  const Note& note = pattern_[index_];
  if (note.frequency_hz == 0) {
    noTone(board::kBuzzerPin);
  } else {
    tone(board::kBuzzerPin, note.frequency_hz);
  }
}

void Outputs::update(uint32_t now_ms) {
  if (pattern_ == nullptr) {
    return;
  }
  if (pending_start_) {
    pending_start_ = false;
    start_note(now_ms);
    return;
  }
  if (now_ms - note_started_ms_ < pattern_[index_].duration_ms) {
    return;
  }
  if (++index_ >= pattern_length_) {
    noTone(board::kBuzzerPin);
    digitalWrite(board::kBuzzerPin, LOW);
    pattern_ = nullptr;
    return;
  }
  start_note(now_ms);
}

void Outputs::show_banner(core::Banner banner) {
  if (led_known_ && banner == led_banner_) {
    return;  // show() briefly blocks interrupts, so only call it on changes
  }
  led_known_ = true;
  led_banner_ = banner;
  uint32_t color = 0;
  switch (banner) {
    case core::Banner::Red:
      color = Adafruit_NeoPixel::Color(64, 0, 0);
      break;
    case core::Banner::Yellow:
      color = Adafruit_NeoPixel::Color(48, 32, 0);
      break;
    case core::Banner::Partial:
      color = Adafruit_NeoPixel::Color(32, 0, 0);
      break;
    case core::Banner::NoData:
      color = Adafruit_NeoPixel::Color(24, 8, 0);
      break;
    case core::Banner::None:
      break;
  }
  led_.setPixelColor(0, color);
  led_.show();
}
