#pragma once

#include <Adafruit_NeoPixel.h>

#include "app.h"
#include "board.h"
#include "screen.h"

// Backlight, buzzer and the RGB LED under the tube. Sound patterns advance without delays.
class Outputs {
 public:
  void begin();
  void set_backlight(uint8_t percent);
  void play(core::Sound sound);              // replaces any pattern still playing
  void show_banner(core::Banner banner);     // LED colour for the banner
  void update(uint32_t now_ms);              // advances the buzzer pattern
  bool busy() const { return pattern_ != nullptr; }  // a pattern is playing

  struct Note {
    uint16_t frequency_hz;  // 0 = silence
    uint16_t duration_ms;
  };

 private:
  void start_note(uint32_t now_ms);

  Adafruit_NeoPixel led_{1, board::kLedPin, NEO_GRB + NEO_KHZ800};
  const Note* pattern_ = nullptr;
  size_t pattern_length_ = 0;
  size_t index_ = 0;
  uint32_t note_started_ms_ = 0;
  bool pending_start_ = false;
  bool led_known_ = false;
  core::Banner led_banner_ = core::Banner::None;
};
