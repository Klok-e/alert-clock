#include "scene.h"

#include <cmath>
#include <cstdio>

namespace ui {
namespace {

using core::Banner;
using core::Panel;
using core::ThreatKind;
using core::ThreatMarker;
using core::ViewModel;

constexpr int16_t kSize = core::kScreenPx;
constexpr int16_t kCenter = kSize / 2;
constexpr int16_t kClockBannerHeight = 56;
constexpr int16_t kMapBannerHeight = 22;
constexpr int16_t kFooterHeight = 14;

// Frames pass through an RGB332 strip, so greys are picked from values RGB332 holds
// exactly; other greys pick up a colour cast (blue has only 2 bits).
constexpr uint16_t kBlack = lgfx::color565(0, 0, 0);
constexpr uint16_t kWhite = lgfx::color565(255, 255, 255);
constexpr uint16_t kGrey = lgfx::color565(146, 146, 170);
constexpr uint16_t kDarkGrey = lgfx::color565(73, 73, 85);
constexpr uint16_t kOrange = lgfx::color565(255, 140, 0);
constexpr uint16_t kAmber = lgfx::color565(255, 200, 0);
constexpr uint16_t kRed = lgfx::color565(210, 0, 0);
constexpr uint16_t kDarkRed = lgfx::color565(130, 0, 0);

const char* const kWeekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Drawing context for one strip: translates scene coordinates and skips shapes
// that cannot touch the strip.
struct Canvas {
  lgfx::LovyanGFX& g;
  int16_t top;

  bool touches(int32_t y, int32_t h) const { return y + h > top && y < top + g.height(); }
  int32_t y(int32_t scene_y) const { return scene_y - top; }
};

struct BannerStyle {
  uint16_t background;
  uint16_t text;
  const char* long_text;
  const char* short_text;
};

bool banner_style(Banner banner, BannerStyle& style) {
  switch (banner) {
    case Banner::Red:
      style = {kRed, kWhite, "AIR RAID ALERT", "ALERT"};
      return true;
    case Banner::Yellow:
      style = {kAmber, kBlack, "THREAT WARNING", "WARNING"};
      return true;
    case Banner::Partial:
      style = {kDarkRed, kWhite, "ALERT IN REGION", "REGION ALERT"};
      return true;
    case Banner::NoData:
      style = {kOrange, kBlack, "NO ALERT DATA", "NO DATA"};
      return true;
    case Banner::None:
      break;
  }
  return false;
}

uint16_t dim(uint16_t color) {
  // Halves each RGB565 channel.
  return static_cast<uint16_t>((color >> 1) & 0x7BEF);
}

uint16_t kind_color(ThreatKind kind) {
  switch (kind) {
    case ThreatKind::Uav:
      return lgfx::color565(255, 140, 0);
    case ThreatKind::Recon:
      return lgfx::color565(0, 200, 255);
    case ThreatKind::Fpv:
      return lgfx::color565(255, 190, 90);
    case ThreatKind::Missile:
      return lgfx::color565(255, 40, 40);
    case ThreatKind::Ballistic:
      return lgfx::color565(255, 0, 210);
    case ThreatKind::Kab:
      return lgfx::color565(255, 230, 0);
    case ThreatKind::Aircraft:
      return kWhite;
    case ThreatKind::Unknown:
      break;
  }
  return kGrey;
}

void draw_banner(Canvas& c, Banner banner, int16_t height, bool long_form) {
  BannerStyle style{};
  if (!banner_style(banner, style) || !c.touches(0, height)) {
    return;
  }
  c.g.fillRect(0, c.y(0), kSize, height, style.background);
  c.g.setTextColor(style.text);
  c.g.setTextDatum(lgfx::textdatum_t::middle_center);
  c.g.setFont(long_form ? &lgfx::fonts::FreeSansBold12pt7b : &lgfx::fonts::FreeSansBold9pt7b);
  c.g.drawString(long_form ? style.long_text : style.short_text, kCenter, c.y(height / 2));
}

void format_age(char* out, size_t size, uint32_t seconds) {
  if (seconds == core::kNever) {
    std::snprintf(out, size, "never");
  } else if (seconds < 120) {
    std::snprintf(out, size, "%us", static_cast<unsigned>(seconds));
  } else if (seconds < 7200) {
    std::snprintf(out, size, "%um", static_cast<unsigned>(seconds / 60));
  } else {
    std::snprintf(out, size, "%uh", static_cast<unsigned>(seconds / 3600));
  }
}

void draw_clock_panel(Canvas& c, const ViewModel& view) {
  draw_banner(c, view.banner, kClockBannerHeight, true);

  const int16_t time_y = 116;
  if (c.touches(time_y - 40, 80)) {
    char text[8];
    if (view.time_valid) {
      const char separator = (view.local.second % 2 == 0) ? ':' : ' ';
      std::snprintf(text, sizeof(text), "%02u%c%02u", view.local.hour, separator, view.local.minute);
    } else {
      std::snprintf(text, sizeof(text), "--:--");
    }
    c.g.setFont(&lgfx::fonts::Font7);
    c.g.setTextSize(1.4f);
    c.g.setTextColor(kWhite);
    c.g.setTextDatum(lgfx::textdatum_t::middle_center);
    c.g.drawString(text, kCenter, c.y(time_y));
    c.g.setTextSize(1.0f);
  }

  const int16_t date_y = 172;
  if (c.touches(date_y - 12, 24)) {
    char text[24];
    if (view.time_valid) {
      std::snprintf(text, sizeof(text), "%s %u %s", kWeekdays[view.local.weekday % 7],
                    view.local.day, kMonths[(view.local.month + 11) % 12]);
    } else {
      std::snprintf(text, sizeof(text), "waiting for time");
    }
    c.g.setFont(&lgfx::fonts::FreeSans9pt7b);
    c.g.setTextColor(kGrey);
    c.g.setTextDatum(lgfx::textdatum_t::middle_center);
    c.g.drawString(text, kCenter, c.y(date_y));
  }

  const int16_t status_y = 212;
  if (c.touches(status_y - 10, 20)) {
    char text[40];
    uint16_t color = kGrey;
    if (!view.threats_fresh) {
      char age[12];
      format_age(age, sizeof(age), view.threats_age_s);
      std::snprintf(text, sizeof(text), "map data: %s old", age);
      color = kOrange;
    } else if (view.area_only_in_region > 0) {
      std::snprintf(text, sizeof(text), "%u in region, no position",
                    static_cast<unsigned>(view.area_only_in_region));
      color = kAmber;
    } else if (view.nearest_km >= 0.0f) {
      std::snprintf(text, sizeof(text), "nearest track %d km",
                    static_cast<int>(std::lround(view.nearest_km)));
    } else {
      text[0] = '\0';
    }
    c.g.setFont(&lgfx::fonts::Font2);
    c.g.setTextColor(color);
    c.g.setTextDatum(lgfx::textdatum_t::middle_center);
    c.g.drawString(text, kCenter, c.y(status_y));
  }
  // Every status line above comes from NEPTUN, whose terms require a visible credit.
  const int16_t credit_y = 231;
  if (c.touches(credit_y - 4, 9)) {
    c.g.setFont(&lgfx::fonts::Font0);
    c.g.setTextColor(kGrey);
    c.g.setTextDatum(lgfx::textdatum_t::middle_center);
    c.g.drawString("data: NEPTUN", kCenter, c.y(credit_y));
  }
}

void draw_marker(Canvas& c, const ThreatMarker& marker) {
  const int32_t x = marker.point.x;
  const int32_t y = marker.point.y;
  uint16_t color = marker.advisory ? kGrey : kind_color(marker.kind);
  if (marker.stale) {
    color = dim(color);
  }
  if (marker.uncertainty_px > 0 && c.touches(y - marker.uncertainty_px, 2 * marker.uncertainty_px + 1)) {
    c.g.drawCircle(x, c.y(y), marker.uncertainty_px, dim(dim(color)));
  }
  if (!c.touches(y - 9, 19)) {
    return;
  }
  if (marker.heading_deg < 0) {
    if (marker.advisory) {
      c.g.drawCircle(x, c.y(y), 4, color);
    } else {
      c.g.fillCircle(x, c.y(y), 4, color);
    }
    return;
  }
  const float heading = static_cast<float>(marker.heading_deg) * 3.14159265f / 180.0f;
  const float wing = 2.44f;  // 140 degrees, in radians
  const int32_t tip_x = x + static_cast<int32_t>(std::lround(8.0f * std::sin(heading)));
  const int32_t tip_y = y - static_cast<int32_t>(std::lround(8.0f * std::cos(heading)));
  const int32_t left_x = x + static_cast<int32_t>(std::lround(6.0f * std::sin(heading + wing)));
  const int32_t left_y = y - static_cast<int32_t>(std::lround(6.0f * std::cos(heading + wing)));
  const int32_t right_x = x + static_cast<int32_t>(std::lround(6.0f * std::sin(heading - wing)));
  const int32_t right_y = y - static_cast<int32_t>(std::lround(6.0f * std::cos(heading - wing)));
  if (marker.advisory) {
    c.g.drawTriangle(tip_x, c.y(tip_y), left_x, c.y(left_y), right_x, c.y(right_y), color);
  } else {
    c.g.fillTriangle(tip_x, c.y(tip_y), left_x, c.y(left_y), right_x, c.y(right_y), color);
  }
}

void draw_map_panel(Canvas& c, const ViewModel& view) {
  c.g.setFont(&lgfx::fonts::Font0);
  c.g.setTextDatum(lgfx::textdatum_t::bottom_center);
  for (const uint8_t km : view.ring_km) {
    const int32_t radius = static_cast<int32_t>(std::lround(static_cast<float>(km) * view.px_per_km));
    c.g.drawCircle(kCenter, c.y(kCenter), radius, kDarkGrey);
    char label[8];
    std::snprintf(label, sizeof(label), "%u", static_cast<unsigned>(km));
    c.g.setTextColor(kDarkGrey);
    c.g.drawString(label, kCenter, c.y(kCenter - radius - 1));
  }
  if (c.touches(kCenter - 6, 13)) {
    c.g.drawFastHLine(kCenter - 6, c.y(kCenter), 13, kWhite);
    c.g.drawFastVLine(kCenter, c.y(kCenter - 6), 13, kWhite);
  }
  for (const ThreatMarker& marker : view.markers) {
    draw_marker(c, marker);
  }

  draw_banner(c, view.banner, kMapBannerHeight, false);
  const int16_t clock_y = view.banner == Banner::None ? 0 : kMapBannerHeight;
  if (c.touches(clock_y, 16)) {
    char text[8];
    if (view.time_valid) {
      std::snprintf(text, sizeof(text), "%02u:%02u", view.local.hour, view.local.minute);
    } else {
      std::snprintf(text, sizeof(text), "--:--");
    }
    c.g.setFont(&lgfx::fonts::Font2);
    c.g.setTextColor(kWhite);
    c.g.setTextDatum(lgfx::textdatum_t::top_right);
    c.g.drawString(text, kSize - 4, c.y(clock_y + 2));
    c.g.setTextDatum(lgfx::textdatum_t::top_left);
    c.g.drawString("N", 4, c.y(clock_y + 2));
  }

  const int16_t footer_y = kSize - kFooterHeight;
  if (c.touches(footer_y, kFooterHeight)) {
    char text[32];
    uint16_t color = kGrey;
    if (!view.threats_fresh) {
      char age[12];
      format_age(age, sizeof(age), view.threats_age_s);
      std::snprintf(text, sizeof(text), "DATA %s OLD", age);
      color = kOrange;
    } else {
      std::snprintf(text, sizeof(text), "%u near", static_cast<unsigned>(view.threats_in_watch));
    }
    c.g.setFont(&lgfx::fonts::Font0);
    c.g.setTextDatum(lgfx::textdatum_t::bottom_left);
    c.g.setTextColor(color);
    c.g.drawString(text, 4, c.y(kSize - 3));
    // NEPTUN's terms require a visible credit next to its data.
    c.g.setTextDatum(lgfx::textdatum_t::bottom_right);
    c.g.setTextColor(kGrey);
    c.g.drawString("data: NEPTUN", kSize - 4, c.y(kSize - 3));
  }
}

}  // namespace

void draw_scene(lgfx::LovyanGFX& g, int16_t top, const ViewModel& view) {
  Canvas canvas{g, top};
  if (view.panel == Panel::Map) {
    draw_map_panel(canvas, view);
  } else {
    draw_clock_panel(canvas, view);
  }
}

void render(lgfx::LovyanGFX& target, lgfx::LGFX_Sprite& band, const ViewModel& view) {
  target.startWrite();
  for (int16_t top = 0; top < kSize; top = static_cast<int16_t>(top + kBandHeight)) {
    band.fillScreen(kBlack);
    draw_scene(band, top, view);
    band.pushSprite(&target, 0, top);
  }
  target.endWrite();
}

}  // namespace ui
