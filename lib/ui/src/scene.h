#pragma once

#include "gfx.h"

#include "view.h"

namespace ui {

// Height of the strip the scene is drawn through. A full 240x240 RGB565 frame (115 KB)
// does not fit the ESP8266, so frames are composed one 240 x kBandHeight RGB565 strip
// (1.9 KB) at a time. Full colour keeps the dark tones of the palette apart.
constexpr int16_t kBandHeight = 4;

// Draws the scene into `g`, shifted up by `top` rows. Drawing outside `g` is clipped.
void draw_scene(lgfx::LovyanGFX& g, int16_t top, const core::ViewModel& view);

// Composes the frame strip by strip through `band` (kScreenPx x kBandHeight, 16-bit)
// into `target`: the panel on the clock, a canvas sprite on the PC.
void render(lgfx::LovyanGFX& target, lgfx::LGFX_Sprite& band, const core::ViewModel& view);

}  // namespace ui
