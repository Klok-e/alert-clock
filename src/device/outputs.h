#pragma once

#include <Adafruit_NeoPixel.h>
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
  Adafruit_NeoPixel led_{1, board::kLedPin, NEO_GRB + NEO_KHZ800};
};
