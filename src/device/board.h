#pragma once

#include <SPI.h>  // SPI_MODE3

#include "gfx.h"

// Hardware of the HG V3.2.2 clock, as recovered from the supplier firmware (its TFT_eSPI
// User_Setup.h and setup()).
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

// The supplier's ST7789 porch, power, VCOM and gamma settings (TFT_eSPI ST7789_2_Init.h in
// the supplier firmware), in place of LovyanGFX's generic values; LovyanGFX's generic list was not tried
// once SPI mode 3 was in place. COLMOD, MADCTL and INVON are sent by LovyanGFX after this list.
class Panel_HG322 : public lgfx::Panel_ST7789 {
 protected:
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        CMD_SLPOUT, CMD_INIT_DELAY, 255,  // 500 ms
        0xB2, 5, 0x1F, 0x1F, 0x00, 0x33, 0x33,  // porch
        0xB7, 1, 0x00,                          // gate voltages
        0xBB, 1, 0x36,                          // VCOM
        0xC2, 1, 0x01,
        0xC3, 1, 0x13,                          // VRH
        0xC4, 1, 0x20,                          // VDV
        0xC6, 1, 0x13,                          // frame rate
        0xD6, 1, 0xA1,
        0xD0, 2, 0xA4, 0xA1,                    // power
        0xE0, 14, 0xF0, 0x08, 0x0E, 0x09, 0x08, 0x04, 0x2F, 0x33, 0x45, 0x36, 0x13, 0x12, 0x2A, 0x2D,
        0xE1, 14, 0xF0, 0x0E, 0x12, 0x0C, 0x0A, 0x15, 0x2E, 0x32, 0x44, 0x39, 0x17, 0x18, 0x2B, 0x2F,
        0xE4, 3, 0x1D, 0x00, 0x00,              // gate lines
        CMD_NORON, CMD_INIT_DELAY, 10,
        CMD_DISPON, CMD_INIT_DELAY, 255,
        0xFF, 0xFF,
    };
    return listno == 0 ? list0 : nullptr;
  }
};

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
      // Mode 3 (clock idles high), as the supplier's TFT_eSPI drives this panel; in mode 0 the
      // panel stayed flat blue. LovyanGFX passes spi_mode straight to the ESP8266 core's
      // SPI.setDataMode(), which reads CPOL from bit 0x10, so it needs SPI_MODE3 (0x11), not 3.
      cfg.spi_mode = SPI_MODE3;
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
      // RGB: MADCTL's BGR bit clear, as the supplier's MADCTL 0x00; false turns orange blue.
      cfg.rgb_order = true;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    setPanel(&panel_);
  }

 private:
  lgfx::Bus_SPI bus_;
  Panel_HG322 panel_;
};
