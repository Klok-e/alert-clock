#include "scene.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP8266)
#include <pgmspace.h>
#endif

namespace ui {
namespace {

using core::Banner;
using core::Panel;
using core::ThreatKind;
using core::ThreatMarker;
using core::ViewModel;

constexpr int16_t kSize = core::kScreenPx;
constexpr int16_t kCenter = kSize / 2;

// Palette after alerts.in.ua: navy ground, amber for drones, wine red for alerts.
struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

constexpr uint16_t to565(Rgb c) { return lgfx::color565(c.r, c.g, c.b); }

// `percent` of the way from `from` to `to`.
constexpr Rgb mix(Rgb from, Rgb to, int percent) {
  return Rgb{static_cast<uint8_t>(from.r + (to.r - from.r) * percent / 100),
             static_cast<uint8_t>(from.g + (to.g - from.g) * percent / 100),
             static_cast<uint8_t>(from.b + (to.b - from.b) * percent / 100)};
}

constexpr Rgb kNavy{27, 33, 49};
constexpr Rgb kCardRgb{35, 44, 63};
constexpr Rgb kLineRgb{50, 61, 85};
constexpr Rgb kWhiteRgb{255, 255, 255};
constexpr Rgb kMutedRgb{142, 152, 173};
constexpr Rgb kAmberRgb{224, 164, 49};
constexpr Rgb kMaroonRgb{139, 34, 54};
constexpr Rgb kAlertRedRgb{192, 38, 63};
constexpr Rgb kOrangeRgb{240, 136, 40};
constexpr Rgb kCyanRgb{63, 198, 220};
constexpr Rgb kGreyRgb{125, 135, 156};

constexpr uint16_t kBackground = to565(kNavy);
constexpr uint16_t kCard = to565(kCardRgb);
constexpr uint16_t kLine = to565(kLineRgb);
constexpr uint16_t kRing = to565(mix(kNavy, kWhiteRgb, 14));
constexpr uint16_t kWhite = to565(kWhiteRgb);
constexpr uint16_t kMuted = to565(kMutedRgb);
constexpr uint16_t kFaint = to565(mix(kNavy, kMutedRgb, 55));
constexpr uint16_t kOrange = to565(kOrangeRgb);

constexpr int16_t kPillHeight = 22;
constexpr int16_t kBadgeRadius = 7;
constexpr int16_t kFooterHeight = 18;

// In flash on the clock, where plain literals occupy RAM; read with copy_name().
#if !defined(ARDUINO_ARCH_ESP8266)
#define PROGMEM
#endif
constexpr size_t kNameLength = 10;  // "Wednesday", "September" and the terminator
const char kWeekdays[7][kNameLength] PROGMEM = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                "Thursday", "Friday", "Saturday"};
const char kMonths[12][kNameLength] PROGMEM = {"January", "February", "March",     "April",
                                               "May",     "June",     "July",      "August",
                                               "September", "October", "November", "December"};

void copy_name(char (&out)[kNameLength], const char* name) {
#if defined(ARDUINO_ARCH_ESP8266)
  strncpy_P(out, name, kNameLength - 1);
#else
  std::strncpy(out, name, kNameLength - 1);
#endif
  out[kNameLength - 1] = '\0';
}

// Drawing context for one strip: translates scene coordinates and skips shapes
// that cannot touch the strip.
struct Canvas {
  lgfx::LovyanGFX& g;
  int16_t top;

  bool touches(int32_t y, int32_t h) const { return y + h > top && y < top + g.height(); }
  int32_t y(int32_t scene_y) const { return scene_y - top; }
};

struct BannerStyle {
  Rgb background;
  uint16_t text;
  const char* label;
};

// The banner's pill. Banner::None has a calm style on the clock face and no pill on the map.
BannerStyle banner_style(Banner banner) {
  switch (banner) {
    case Banner::Red:
      return {kMaroonRgb, kWhite, "Air raid alert"};
    case Banner::Yellow:
      return {kAmberRgb, kBackground, "Threat warning"};
    case Banner::Partial:
      return {mix(kMaroonRgb, kNavy, 35), kWhite, "Alert in region"};
    case Banner::NoData:
      return {kOrangeRgb, kBackground, "No alert data"};
    case Banner::None:
      break;
  }
  return {kCardRgb, kMuted, "No alert"};
}

// The map ground takes the banner's colour faintly, as alerts.in.ua fills an alerted region.
uint16_t map_ground(Banner banner) {
  switch (banner) {
    case Banner::Red:
      return to565(mix(kNavy, kMaroonRgb, 30));
    case Banner::Partial:
      return to565(mix(kNavy, kMaroonRgb, 18));
    case Banner::Yellow:
      return to565(mix(kNavy, kAmberRgb, 10));
    case Banner::NoData:
    case Banner::None:
      break;
  }
  return kBackground;
}

Rgb kind_rgb(ThreatKind kind) {
  switch (kind) {
    case ThreatKind::Uav:
      return kAmberRgb;
    case ThreatKind::Recon:
      return kCyanRgb;
    case ThreatKind::Fpv:
      return Rgb{240, 200, 120};
    case ThreatKind::Missile:
      return kAlertRedRgb;
    case ThreatKind::Ballistic:
      return Rgb{205, 70, 200};
    case ThreatKind::Kab:
      return Rgb{245, 220, 70};
    case ThreatKind::Aircraft:
      return kWhiteRgb;
    case ThreatKind::Unknown:
      break;
  }
  return kGreyRgb;
}

const char* kind_label(ThreatKind kind) {
  switch (kind) {
    case ThreatKind::Uav:
      return "UAV";
    case ThreatKind::Recon:
      return "Recon";
    case ThreatKind::Fpv:
      return "FPV";
    case ThreatKind::Missile:
      return "Missile";
    case ThreatKind::Ballistic:
      return "Ballistic";
    case ThreatKind::Kab:
      return "KAB";
    case ThreatKind::Aircraft:
      return "Aircraft";
    case ThreatKind::Unknown:
      break;
  }
  return "Other";
}

// Legend order: the most dangerous kinds first.
constexpr ThreatKind kLegendOrder[] = {ThreatKind::Ballistic, ThreatKind::Missile, ThreatKind::Kab,
                                       ThreatKind::Uav,       ThreatKind::Fpv,     ThreatKind::Recon,
                                       ThreatKind::Aircraft,  ThreatKind::Unknown};
constexpr size_t kKinds = sizeof(kLegendOrder) / sizeof(kLegendOrder[0]);

// A marker's colour after fading for staleness and greying for advisories.
Rgb marker_rgb(const ThreatMarker& marker, Rgb ground) {
  const Rgb base = marker.advisory ? kGreyRgb : kind_rgb(marker.kind);
  return marker.stale ? mix(base, ground, 50) : base;
}

Rgb rgb_from565(uint16_t color) {
  return Rgb{static_cast<uint8_t>(((color >> 11) & 0x1F) * 255 / 31),
             static_cast<uint8_t>(((color >> 5) & 0x3F) * 255 / 63),
             static_cast<uint8_t>((color & 0x1F) * 255 / 31)};
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

// A rounded pill with a dot and a label; returns its width. `x` is the left edge, or the
// centre when `centered`.
int16_t draw_pill(Canvas& c, int32_t x, int32_t top, const BannerStyle& style, bool centered) {
  c.g.setFont(&lgfx::fonts::FreeSansBold9pt7b);
  const int32_t text_width = c.g.textWidth(style.label);
  const int32_t width = 10 + 6 + 7 + text_width + 12;
  const int32_t left = centered ? x - width / 2 : x;
  if (c.touches(top, kPillHeight)) {
    c.g.fillRoundRect(left, c.y(top), width, kPillHeight, kPillHeight / 2, to565(style.background));
    c.g.fillCircle(left + 13, c.y(top + kPillHeight / 2), 3, style.text);
    c.g.setTextColor(style.text);
    c.g.setTextDatum(lgfx::textdatum_t::middle_left);
    c.g.drawString(style.label, left + 23, c.y(top + kPillHeight / 2));
  }
  return static_cast<int16_t>(width);
}

// A filled disc with an arrow along the heading, or a dot when the heading is unknown.
void draw_badge(Canvas& c, int32_t x, int32_t y, int16_t heading_deg, Rgb fill, bool hollow, uint16_t ground) {
  if (!c.touches(y - kBadgeRadius - 1, 2 * kBadgeRadius + 3)) {
    return;
  }
  const uint16_t color = to565(fill);
  const bool bright = 30 * fill.r + 59 * fill.g + 11 * fill.b > 14000;  // luma above ~140
  const uint16_t glyph = hollow ? color : (bright ? kBackground : kWhite);
  if (hollow) {
    c.g.fillCircle(x, c.y(y), kBadgeRadius + 1, ground);
    c.g.drawCircle(x, c.y(y), kBadgeRadius, color);
  } else {
    c.g.fillCircle(x, c.y(y), kBadgeRadius + 1, bright ? ground : kWhite);
    c.g.fillCircle(x, c.y(y), kBadgeRadius, color);
  }
  if (heading_deg < 0) {
    c.g.fillCircle(x, c.y(y), 2, glyph);
    return;
  }
  const float heading = static_cast<float>(heading_deg) * 3.14159265f / 180.0f;
  const float wing = 2.5f;  // about 143 degrees, in radians
  const int32_t tip_x = x + static_cast<int32_t>(std::lround(5.0f * std::sin(heading)));
  const int32_t tip_y = y - static_cast<int32_t>(std::lround(5.0f * std::cos(heading)));
  const int32_t left_x = x + static_cast<int32_t>(std::lround(4.0f * std::sin(heading + wing)));
  const int32_t left_y = y - static_cast<int32_t>(std::lround(4.0f * std::cos(heading + wing)));
  const int32_t right_x = x + static_cast<int32_t>(std::lround(4.0f * std::sin(heading - wing)));
  const int32_t right_y = y - static_cast<int32_t>(std::lround(4.0f * std::cos(heading - wing)));
  c.g.fillTriangle(tip_x, c.y(tip_y), left_x, c.y(left_y), right_x, c.y(right_y), glyph);
}

void draw_time(Canvas& c, const ViewModel& view, int32_t baseline) {
  c.g.setFont(&lgfx::fonts::DejaVu72);
  c.g.setTextColor(kWhite);
  if (!view.time_valid) {
    c.g.setTextDatum(lgfx::textdatum_t::baseline_center);
    c.g.drawString("--:--", kCenter, c.y(baseline));
    return;
  }
  // Hours and minutes are placed around a fixed colon, so the blink never shifts them.
  char hours[4];
  char minutes[4];
  std::snprintf(hours, sizeof(hours), "%02u", view.local.hour);
  std::snprintf(minutes, sizeof(minutes), "%02u", view.local.minute);
  const int32_t half_colon = c.g.textWidth(":") / 2;
  c.g.setTextDatum(lgfx::textdatum_t::baseline_right);
  c.g.drawString(hours, kCenter - half_colon, c.y(baseline));
  c.g.setTextDatum(lgfx::textdatum_t::baseline_left);
  c.g.drawString(minutes, kCenter + half_colon, c.y(baseline));
  if (view.local.second % 2 == 0) {
    c.g.setTextDatum(lgfx::textdatum_t::baseline_center);
    c.g.drawString(":", kCenter, c.y(baseline - 4));
  }
}

// The card under the clock: what NEPTUN shows near home.
void draw_threat_card(Canvas& c, const ViewModel& view, int32_t top) {
  constexpr int32_t kLeft = 14;
  constexpr int32_t kWidth = kSize - 2 * kLeft;
  constexpr int32_t kHeight = 48;
  if (!c.touches(top, kHeight)) {
    return;
  }
  char title[32];
  char detail[40];
  Rgb badge = kGreyRgb;
  uint16_t title_color = kWhite;
  int16_t heading = -1;
  bool hollow = false;
  if (!view.threats_fresh) {
    char age[12];
    format_age(age, sizeof(age), view.threats_age_s);
    std::snprintf(title, sizeof(title), "Map data %s old", age);
    std::snprintf(detail, sizeof(detail), "tracks may have moved");
    badge = kOrangeRgb;
    title_color = kOrange;
  } else if (view.area_only_in_region > 0) {
    std::snprintf(title, sizeof(title), "%u in your region", static_cast<unsigned>(view.area_only_in_region));
    std::snprintf(detail, sizeof(detail), "position not reported");
    badge = kAmberRgb;
  } else if (view.nearest_km >= 0.0f) {
    const int km = static_cast<int>(std::lround(view.nearest_km));
    if (view.threats_in_watch > 0) {
      std::snprintf(title, sizeof(title), "%u nearby", static_cast<unsigned>(view.threats_in_watch));
    } else {
      std::snprintf(title, sizeof(title), "Nothing close");
    }
    // Advisories are observations, not shelter signals: hollow, and named as such.
    std::snprintf(detail, sizeof(detail), view.nearest_advisory ? "nearest advisory %d km" : "nearest track %d km",
                  km);
    ThreatMarker nearest;
    nearest.kind = view.nearest_kind;
    nearest.advisory = view.nearest_advisory;
    nearest.stale = view.nearest_stale;
    badge = marker_rgb(nearest, kCardRgb);
    heading = view.nearest_heading_deg;
    hollow = view.nearest_advisory;
  } else {
    char age[12];
    format_age(age, sizeof(age), view.threats_age_s);
    std::snprintf(title, sizeof(title), "No tracks nearby");
    std::snprintf(detail, sizeof(detail), "updated %s ago", age);
    title_color = kMuted;
  }

  c.g.fillRoundRect(kLeft, c.y(top), kWidth, kHeight, 10, kCard);
  c.g.drawRoundRect(kLeft, c.y(top), kWidth, kHeight, 10, kLine);
  draw_badge(c, kLeft + 22, top + kHeight / 2, heading, badge, hollow, kCard);
  c.g.setTextDatum(lgfx::textdatum_t::baseline_left);
  c.g.setFont(&lgfx::fonts::FreeSansBold9pt7b);
  c.g.setTextColor(title_color);
  c.g.drawString(title, kLeft + 40, c.y(top + 21));
  c.g.setFont(&lgfx::fonts::Font2);
  c.g.setTextColor(kMuted);
  c.g.setTextDatum(lgfx::textdatum_t::top_left);
  c.g.drawString(detail, kLeft + 40, c.y(top + 26));
}

void draw_clock_panel(Canvas& c, const ViewModel& view) {
  c.g.fillScreen(kBackground);
  draw_pill(c, kCenter, 12, banner_style(view.banner), true);

  const int32_t time_baseline = 112;
  if (c.touches(time_baseline - 56, 60)) {
    draw_time(c, view, time_baseline);
  }

  const int16_t date_y = 140;
  if (c.touches(date_y - 12, 24)) {
    char text[32];
    if (view.time_valid) {
      char weekday[kNameLength];
      char month[kNameLength];
      copy_name(weekday, kWeekdays[view.local.weekday % 7]);
      copy_name(month, kMonths[(view.local.month + 11) % 12]);
      std::snprintf(text, sizeof(text), "%s, %u %s", weekday, view.local.day, month);
    } else {
      std::snprintf(text, sizeof(text), "waiting for time");
    }
    c.g.setFont(&lgfx::fonts::FreeSans9pt7b);
    c.g.setTextColor(kMuted);
    c.g.setTextDatum(lgfx::textdatum_t::middle_center);
    c.g.drawString(text, kCenter, c.y(date_y));
  }

  draw_threat_card(c, view, 162);

  // The card shows NEPTUN data, whose terms require a visible credit.
  const int16_t credit_y = 228;
  if (c.touches(credit_y - 4, 9)) {
    c.g.setFont(&lgfx::fonts::Font0);
    c.g.setTextColor(kMuted);
    c.g.setTextDatum(lgfx::textdatum_t::middle_center);
    c.g.drawString("data: NEPTUN", kCenter, c.y(credit_y));
  }
}

// Coloured squares with a count per kind, most dangerous first, as many as fit before `right`.
void draw_legend(Canvas& c, const ViewModel& view, int32_t baseline, int32_t right) {
  uint16_t counts[kKinds] = {};
  for (const ThreatMarker& marker : view.markers) {
    if (marker.advisory) {
      continue;
    }
    for (size_t i = 0; i < kKinds; ++i) {
      if (kLegendOrder[i] == marker.kind) {
        ++counts[i];
        break;
      }
    }
  }
  c.g.setFont(&lgfx::fonts::Font0);
  c.g.setTextColor(kWhite);
  c.g.setTextDatum(lgfx::textdatum_t::baseline_left);
  int32_t x = 6;
  for (size_t i = 0; i < kKinds; ++i) {
    if (counts[i] == 0) {
      continue;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%s %u", kind_label(kLegendOrder[i]), static_cast<unsigned>(counts[i]));
    const int32_t width = 11 + c.g.textWidth(text);
    if (x + width > right) {
      break;
    }
    c.g.fillRoundRect(x, c.y(baseline - 7), 7, 7, 2, to565(kind_rgb(kLegendOrder[i])));
    c.g.drawString(text, x + 11, c.y(baseline));
    x += width + 8;
  }
}

void draw_map_panel(Canvas& c, const ViewModel& view) {
  const uint16_t ground = map_ground(view.banner);
  const Rgb ground_rgb = rgb_from565(ground);
  c.g.fillScreen(ground);

  c.g.setFont(&lgfx::fonts::Font0);
  c.g.setTextDatum(lgfx::textdatum_t::bottom_center);
  c.g.setTextColor(kFaint);
  for (const uint8_t km : view.ring_km) {
    const int32_t radius = static_cast<int32_t>(std::lround(static_cast<float>(km) * view.px_per_km));
    c.g.drawCircle(kCenter, c.y(kCenter), radius, kRing);
    char label[8];
    std::snprintf(label, sizeof(label), "%u km", static_cast<unsigned>(km));
    c.g.drawString(label, kCenter, c.y(kCenter - radius - 1));
  }

  // Uncertainty halos first, so no halo covers a badge.
  for (const ThreatMarker& marker : view.markers) {
    const int32_t r = marker.uncertainty_px;
    if (r <= kBadgeRadius || !c.touches(marker.point.y - r, 2 * r + 1)) {
      continue;
    }
    const Rgb color = marker_rgb(marker, ground_rgb);
    c.g.fillCircle(marker.point.x, c.y(marker.point.y), r, to565(mix(ground_rgb, color, 16)));
    c.g.drawCircle(marker.point.x, c.y(marker.point.y), r, to565(mix(ground_rgb, color, 45)));
  }

  if (c.touches(kCenter - 6, 13)) {
    c.g.drawCircle(kCenter, c.y(kCenter), 5, kWhite);
    c.g.fillCircle(kCenter, c.y(kCenter), 1, kWhite);
  }

  // Farthest first, so the nearest track is drawn on top.
  for (size_t i = view.markers.size(); i-- > 0;) {
    const ThreatMarker& marker = view.markers[i];
    draw_badge(c, marker.point.x, marker.point.y, marker.heading_deg, marker_rgb(marker, ground_rgb),
               marker.advisory, ground);
  }

  // Top: the banner pill, the time pill and north.
  const int32_t pill_top = 6;
  if (view.banner != Banner::None) {
    draw_pill(c, 6, pill_top, banner_style(view.banner), false);
  }
  if (c.touches(pill_top, kPillHeight)) {
    char text[8];
    if (view.time_valid) {
      std::snprintf(text, sizeof(text), "%02u:%02u", view.local.hour, view.local.minute);
    } else {
      std::snprintf(text, sizeof(text), "--:--");
    }
    c.g.setFont(&lgfx::fonts::FreeSansBold9pt7b);
    const int32_t width = c.g.textWidth(text) + 16;
    c.g.fillRoundRect(kSize - 6 - width, c.y(pill_top), width, kPillHeight, kPillHeight / 2, kBackground);
    c.g.setTextColor(kWhite);
    c.g.setTextDatum(lgfx::textdatum_t::middle_center);
    c.g.drawString(text, kSize - 6 - width / 2, c.y(pill_top + kPillHeight / 2));
    if (view.banner == Banner::None) {
      c.g.setFont(&lgfx::fonts::Font2);
      c.g.setTextColor(kMuted);
      c.g.setTextDatum(lgfx::textdatum_t::middle_left);
      c.g.drawString("N", 8, c.y(pill_top + kPillHeight / 2));
    }
  }

  // Footer: legend or data age, and NEPTUN's credit, which its terms require.
  const int16_t footer_y = kSize - kFooterHeight;
  if (c.touches(footer_y, kFooterHeight)) {
    c.g.fillRect(0, c.y(footer_y), kSize, kFooterHeight, kBackground);
    c.g.drawFastHLine(0, c.y(footer_y), kSize, kLine);
    const int32_t baseline = kSize - 6;
    c.g.setFont(&lgfx::fonts::Font0);
    const int32_t credit_width = c.g.textWidth("data: NEPTUN");
    c.g.setTextColor(kMuted);
    c.g.setTextDatum(lgfx::textdatum_t::baseline_right);
    c.g.drawString("data: NEPTUN", kSize - 6, c.y(baseline));
    if (!view.threats_fresh) {
      char age[12];
      char text[24];
      format_age(age, sizeof(age), view.threats_age_s);
      std::snprintf(text, sizeof(text), "Data %s old", age);
      c.g.setTextColor(kOrange);
      c.g.setTextDatum(lgfx::textdatum_t::baseline_left);
      c.g.drawString(text, 6, c.y(baseline));
    } else {
      draw_legend(c, view, baseline, kSize - 12 - credit_width);
    }
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
    draw_scene(band, top, view);
    band.pushSprite(&target, 0, top);
  }
  target.endWrite();
}

}  // namespace ui
