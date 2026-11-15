#pragma once

#include "gfx.h"

// Hardware of the HG V3.2.2 clock, as recovered from the supplier firmware
// (re-nixie-clock: cpp/vendor/tft-espi-16e3759/User_Setup.h, cpp/src/setup.cpp).
namespace board {

constexpr uint8_t kTftSclk = 14;      // HSPI
constexpr uint8_t kTftMosi = 13;      // HSPI
constexpr uint8_t kTftCs = 15;
constexpr uint8_t kTftDc = 0;         // also the boot-mode strap
constexpr uint8_t kTftRst = 2;
constexpr uint8_t kBacklightPin = 5;  // PWM, active low
constexpr uint8_t kBuzzerPin = 4;     // passive piezo, driven with tone()
constexpr uint8_t kButtonPin = 16;    // active low; GPIO16 has no internal pull-up
constexpr uint8_t kLedPin = 12;       // one WS2812 (GRB, 800 kHz); it is also HSPI MISO,
                                      // so the display cannot be read back

}  // namespace board

// ST7789 240x240 IPS on hardware SPI at 40 MHz, colours inverted (supplier: invertDisplay(true)).
class ClockDisplay : public lgfx::LGFX_Device {
 public:
  ClockDisplay() {
    {
      auto cfg = bus_.config();
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.pin_sclk = board::kTftSclk;
      cfg.pin_mosi = board::kTftMosi;
      cfg.pin_miso = -1;
      cfg.pin_dc = board::kTftDc;
      cfg.spi_mode = 0;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = board::kTftCs;
      cfg.pin_rst = board::kTftRst;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;   // ST7789 RAM is 240x320; LovyanGFX applies the
      cfg.memory_height = 320;  // row offset each rotation needs
      cfg.panel_width = 240;
      cfg.panel_height = 240;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    setPanel(&panel_);
  }

 private:
  lgfx::Bus_SPI bus_;
  lgfx::Panel_ST7789 panel_;
};
