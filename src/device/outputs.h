#pragma once

#include <stdint.h>

#include "board.h"
#include "output_policy.h"

class Outputs final : public core::OutputSink {
 public:
  void begin();
  void begin_led();
  void backlight(uint8_t percent) override;
  void buzzer(uint16_t frequency_hz) override;
  void led(uint8_t red, uint8_t green, uint8_t blue) override;
 private:
  uint32_t led_sent_us_ = 0;  // end of the last frame; the WS2812 latches after 300 us low
};
