#include "scene.h"

#include <cmath>
#include <cstdio>

#include "flash.h"
#include "fonts.h"

namespace ui {
namespace {

using core::Banner;
using core::Marker;
using core::Panel;
using core::Snapshot;
using core::ThreatKind;
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
constexpr uint16_t kRing = to565(mix(kNavy, kWhiteRgb, 22));
constexpr uint16_t kWhite = to565(kWhiteRgb);
constexpr uint16_t kMuted = to565(kMutedRgb);
constexpr uint16_t kOrange = to565(kOrangeRgb);

constexpr int16_t kPillHeight = 22;
constexpr int16_t kBadgeRadius = 7;
constexpr int16_t kFooterHeight = 30;

// Text, in flash: on the clock plain literals occupy RAM. Read with flash_copy/flash_format.
const char kCredit[] PROGMEM = "дані: neptun.in.ua · © OpenStreetMap";
const char kNoticeLines[4][40] PROGMEM = {"Неофіційне джерело.", "Орієнтуйтеся на", "офіційні сигнали",
                                         "тривоги."};
const char kNoTime[] PROGMEM = "--:--";
const char kWaitingForTime[] PROGMEM = "очікування часу";
const char kDateFormat[] PROGMEM = "%s, %u %s";
const char kTimeFormat[] PROGMEM = "%02u:%02u";
const char kTwoDigits[] PROGMEM = "%02u";
const char kColon[] PROGMEM = ":";
const char kNorth[] PROGMEM = "Пн";
const char kRingFormat[] PROGMEM = "%u км";
const char kLegendFormat[] PROGMEM = "%s %u";

const char kAgeNever[] PROGMEM = "ніколи";
const char kAgeSeconds[] PROGMEM = "%u с";
const char kAgeMinutes[] PROGMEM = "%u хв";
const char kAgeHours[] PROGMEM = "%u год";

const char kDurationUnderMinute[] PROGMEM = "<1 хв";
const char kDurationMinutes[] PROGMEM = "%u хв";
const char kDurationHours[] PROGMEM = "%u год %u хв";
const char kDurationDays[] PROGMEM = "%u д %u год";
const char kPillWithDuration[] PROGMEM = "%s · %s";

const char kCardStaleTitle[] PROGMEM = "Дані: %s тому";
const char kCardStaleDetail[] PROGMEM = "цілі могли зміститися";
const char kCardRegionTitle[] PROGMEM = "%u у вашій області";
const char kCardRegionDetail[] PROGMEM = "місце не вказане";
const char kCardNearTitle[] PROGMEM = "%u поблизу";
const char kCardFarTitle[] PROGMEM = "Поблизу нічого";
const char kCardNearestTrack[] PROGMEM = "найближча ціль %d км";
const char kCardNearestAdvisory[] PROGMEM = "спостереження %d км";
const char kCardClearTitle[] PROGMEM = "Цілей поблизу немає";
const char kCardClearDetail[] PROGMEM = "оновлено %s тому";
const char kMapStale[] PROGMEM = "Дані %s тому";

constexpr size_t kNameBytes = 20;  // the longest weekday or month in UTF-8, plus the terminator
const char kWeekdays[7][kNameBytes] PROGMEM = {"неділя", "понеділок", "вівторок", "середа",
                                               "четвер", "пʼятниця",  "субота"};
// Genitive, as in "14 листопада".
const char kMonths[12][kNameBytes] PROGMEM = {"січня",  "лютого",  "березня",  "квітня",
                                              "травня", "червня",  "липня",    "серпня",
                                              "вересня", "жовтня", "листопада", "грудня"};
const char kKindLabels[8][kNameBytes] PROGMEM = {"БпЛА", "Розвідка", "FPV",   "Ракета",
                                                 "Баліст.", "КАБ",   "Літак", "Інше"};

constexpr size_t kLabelBytes = 40;
// Long labels for the clock face; short ones for the map, where the time shares the top row.
const char kBannerLong[5][kLabelBytes] PROGMEM = {"Тривоги немає", "Немає даних", "Тривога в області",
                                                  "Загроза", "Повітряна тривога"};
const char kBannerShort[5][kLabelBytes] PROGMEM = {"", "Немає даних", "В області", "Загроза", "Тривога"};

struct BannerStyle {
  Rgb background;
  uint16_t text;
};

// Banner::None has a calm style on the clock face and no pill on the map.
BannerStyle banner_style(Banner banner) {
  switch (banner) {
    case Banner::Red:
      return {kMaroonRgb, kWhite};
    case Banner::Yellow:
      return {kAmberRgb, kBackground};
    case Banner::Partial:
      return {mix(kMaroonRgb, kNavy, 35), kWhite};
    case Banner::NoData:
      return {kOrangeRgb, kBackground};
    case Banner::None:
      break;
  }
  return {kCardRgb, kMuted};
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

// Legend order: the most dangerous kinds first.
constexpr ThreatKind kLegendOrder[] = {ThreatKind::Ballistic, ThreatKind::Missile, ThreatKind::Kab,
                                       ThreatKind::Uav,       ThreatKind::Fpv,     ThreatKind::Recon,
                                       ThreatKind::Aircraft,  ThreatKind::Unknown};
constexpr size_t kKinds = sizeof(kLegendOrder) / sizeof(kLegendOrder[0]);

// A marker's colour after fading for staleness and greying for advisories.
Rgb marker_rgb(ThreatKind kind, bool advisory, bool stale, Rgb ground) {
  const Rgb base = advisory ? kGreyRgb : kind_rgb(kind);
  return stale ? mix(base, ground, 50) : base;
}

void format_age(char* out, size_t size, uint32_t seconds) {
  if (seconds == core::kNever) {
    flash_copy(out, size, kAgeNever);
  } else if (seconds < 120) {
    flash_format(out, size, kAgeSeconds, static_cast<unsigned>(seconds));
  } else if (seconds < 7200) {
    flash_format(out, size, kAgeMinutes, static_cast<unsigned>(seconds / 60));
  } else {
    flash_format(out, size, kAgeHours, static_cast<unsigned>(seconds / 3600));
  }
}

// The two largest units: "<1 хв", "23 хв", "1 год 23 хв", "2 д 3 год".
void format_duration(char* out, size_t size, uint32_t seconds) {
  const unsigned minutes = static_cast<unsigned>(seconds / 60);
  if (minutes == 0) {
    flash_copy(out, size, kDurationUnderMinute);
  } else if (minutes < 60) {
    flash_format(out, size, kDurationMinutes, minutes);
  } else if (minutes < 24 * 60) {
    flash_format(out, size, kDurationHours, minutes / 60, minutes % 60);
  } else {
    flash_format(out, size, kDurationDays, minutes / (24 * 60), minutes / 60 % 24);
  }
}

// Drawing context for one strip: translates scene coordinates and skips shapes
// that cannot touch the strip.
struct Canvas {
  lgfx::LovyanGFX& g;
  int16_t top;

  bool touches(int32_t y, int32_t h) const { return y + h > top && y < top + g.height(); }
  int32_t y(int32_t scene_y) const { return scene_y - top; }

  void text(const char* flash_text, int32_t x, int32_t scene_y, const lgfx::IFont* font, uint16_t color,
            lgfx::textdatum_t datum) {
    char buffer[96];
    flash_copy(buffer, sizeof(buffer), flash_text);
    ram_text(buffer, x, scene_y, font, color, datum);
  }
  void ram_text(const char* text, int32_t x, int32_t scene_y, const lgfx::IFont* font, uint16_t color,
                lgfx::textdatum_t datum) {
    g.setFont(font);
    g.setTextColor(color);
    g.setTextDatum(datum);
    g.drawString(text, x, y(scene_y));
  }
};

constexpr int32_t kPillLeftPadding = 10;
constexpr int32_t kPillDotRadius = 3;
constexpr int32_t kPillDotTextGap = 7;
constexpr int32_t kPillRightPadding = 12;
constexpr int32_t kPillDotCenterX = kPillLeftPadding + kPillDotRadius;
constexpr int32_t kPillTextX = kPillLeftPadding + 2 * kPillDotRadius + kPillDotTextGap;
constexpr int32_t kPillPadding = kPillTextX + kPillRightPadding;

enum class PillLabelStyle { Full, Compact };
enum class HorizontalAnchor { Left, Center };

// The banner label; with a duration (not kNever), the short label and the duration, as
// "Тривога · 1 год 23 хв", or the duration alone, in the pill's colour, if that is wider than
// `max_width`.
void pill_label(Canvas& c, char* out, size_t size, Banner banner, PillLabelStyle label_style, uint32_t duration_s,
                int32_t max_width) {
  const size_t index = static_cast<size_t>(banner) % 5;
  if (duration_s == core::kNever) {
    flash_copy(out, size, label_style == PillLabelStyle::Compact ? kBannerShort[index] : kBannerLong[index]);
    return;
  }
  char label[kLabelBytes];
  char duration[24];
  flash_copy(label, sizeof(label), kBannerShort[index]);
  format_duration(duration, sizeof(duration), duration_s);
  flash_format(out, size, kPillWithDuration, label, duration);
  c.g.setFont(&kInterTitle);
  if (c.g.textWidth(out) + kPillPadding > max_width) {
    std::snprintf(out, size, "%s", duration);
  }
}

void draw_pill(Canvas& c, int32_t x, int32_t top, Banner banner, PillLabelStyle label_style, HorizontalAnchor anchor,
               uint32_t duration_s, int32_t max_width) {
  if (!c.touches(top, kPillHeight)) {
    return;
  }
  const BannerStyle style = banner_style(banner);
  char label[kLabelBytes + 24];
  pill_label(c, label, sizeof(label), banner, label_style, duration_s, max_width);
  c.g.setFont(&kInterTitle);
  const int32_t width = c.g.textWidth(label) + kPillPadding;
  const int32_t left = anchor == HorizontalAnchor::Center ? x - width / 2 : x;
  c.g.fillRoundRect(left, c.y(top), width, kPillHeight, kPillHeight / 2, to565(style.background));
  c.g.fillCircle(left + kPillDotCenterX, c.y(top + kPillHeight / 2), kPillDotRadius, style.text);
  c.ram_text(label, left + kPillTextX, top + kPillHeight / 2, &kInterTitle, style.text, lgfx::textdatum_t::middle_left);
}

uint16_t draw_badge_disc(Canvas& c, int32_t x, int32_t y, Rgb fill, bool hollow,
                         bool stale, uint16_t ground) {
  const uint16_t color = to565(fill);
  const bool bright = 30 * fill.r + 59 * fill.g + 11 * fill.b > 14000;
  const uint16_t light = stale ? to565(mix(kWhiteRgb, kNavy, 50)) : kWhite;
  if (hollow) {
    c.g.fillCircle(x, c.y(y), kBadgeRadius + 1, ground);
    c.g.drawCircle(x, c.y(y), kBadgeRadius, color);
  } else {
    c.g.fillCircle(x, c.y(y), kBadgeRadius + 1, bright ? ground : light);
    c.g.fillCircle(x, c.y(y), kBadgeRadius, color);
  }
  return hollow ? color : (bright ? kBackground : light);
}

void draw_status_badge(Canvas& c, int32_t x, int32_t y, Rgb fill, uint16_t ground) {
  if (c.touches(y - kBadgeRadius - 1, 2 * kBadgeRadius + 3)) {
    const uint16_t glyph = draw_badge_disc(c, x, y, fill, false, false, ground);
    c.g.fillCircle(x, c.y(y), 2, glyph);
  }
}

// Glyphs stay upright: the separate pointer carries bearing without rotating the silhouette.
void draw_kind_glyph(Canvas& c, int32_t x, int32_t y, ThreatKind kind, uint16_t ink) {
  const int32_t cy = c.y(y);
  switch (kind) {
    case ThreatKind::Uav:
      c.g.fillTriangle(x, cy - 5, x - 5, cy + 4, x + 5, cy + 4, ink);
      c.g.drawFastVLine(x, cy + 3, 3, ink);
      break;
    case ThreatKind::Recon:
      c.g.drawEllipse(x, cy, 5, 3, ink);
      c.g.fillCircle(x, cy, 1, ink);
      break;
    case ThreatKind::Fpv:
      c.g.drawLine(x - 3, cy - 3, x + 3, cy + 3, ink);
      c.g.drawLine(x - 3, cy + 3, x + 3, cy - 3, ink);
      c.g.drawCircle(x - 3, cy - 3, 2, ink);
      c.g.drawCircle(x + 3, cy - 3, 2, ink);
      c.g.drawCircle(x - 3, cy + 3, 2, ink);
      c.g.drawCircle(x + 3, cy + 3, 2, ink);
      break;
    case ThreatKind::Missile:
      c.g.fillTriangle(x, cy - 5, x - 1, cy - 2, x + 1, cy - 2, ink);
      c.g.fillRect(x - 1, cy - 2, 3, 6, ink);
      c.g.drawLine(x - 1, cy + 1, x - 3, cy + 4, ink);
      c.g.drawLine(x + 1, cy + 1, x + 3, cy + 4, ink);
      break;
    case ThreatKind::Ballistic:
      c.g.fillTriangle(x + 1, cy - 5, x - 4, cy + 1, x + 1, cy + 1, ink);
      c.g.fillTriangle(x - 1, cy - 1, x + 4, cy - 1, x - 1, cy + 5, ink);
      break;
    case ThreatKind::Kab:
      c.g.fillTriangle(x - 3, cy - 5, x + 3, cy - 5, x, cy - 1, ink);
      c.g.fillEllipse(x, cy + 1, 3, 4, ink);
      break;
    case ThreatKind::Aircraft:
      c.g.fillRect(x - 1, cy - 5, 3, 10, ink);
      c.g.fillTriangle(x, cy - 2, x - 5, cy + 1, x + 5, cy + 1, ink);
      c.g.drawFastHLine(x - 3, cy + 4, 7, ink);
      break;
    case ThreatKind::Unknown:
      c.g.drawLine(x - 2, cy - 4, x + 2, cy - 4, ink);
      c.g.drawLine(x + 2, cy - 4, x + 3, cy - 3, ink);
      c.g.drawLine(x + 3, cy - 3, x + 3, cy - 1, ink);
      c.g.drawLine(x + 3, cy - 1, x, cy + 1, ink);
      c.g.drawFastVLine(x, cy + 1, 2, ink);
      c.g.drawPixel(x, cy + 5, ink);
      break;
  }
}

void draw_threat_badge(Canvas& c, int32_t x, int32_t y, ThreatKind kind,
                       int16_t heading_deg, Rgb fill, bool hollow, bool stale, uint16_t ground) {
  constexpr int32_t kExtent = 20;
  if (!c.touches(y - kExtent, 2 * kExtent + 1)) {
    return;
  }
  const uint16_t glyph = draw_badge_disc(c, x, y, fill, hollow, stale, ground);
  draw_kind_glyph(c, x, y, kind, glyph);
  if (heading_deg < 0) {
    return;
  }
  const float heading = static_cast<float>(heading_deg) * 3.14159265f / 180.0f;
  const float dx = std::sin(heading);
  const float dy = -std::cos(heading);
  const auto triangle = [&](float tip, float base, float half_width, uint16_t color) {
    const int32_t tip_x = x + static_cast<int32_t>(std::lround(tip * dx));
    const int32_t tip_y = y + static_cast<int32_t>(std::lround(tip * dy));
    const int32_t base_x = x + static_cast<int32_t>(std::lround(base * dx));
    const int32_t base_y = y + static_cast<int32_t>(std::lround(base * dy));
    const int32_t side_x = static_cast<int32_t>(std::lround(half_width * dy));
    const int32_t side_y = static_cast<int32_t>(std::lround(-half_width * dx));
    c.g.fillTriangle(tip_x, c.y(tip_y), base_x + side_x, c.y(base_y + side_y),
                     base_x - side_x, c.y(base_y - side_y), color);
  };
  const uint16_t outline = stale ? to565(mix(kWhiteRgb, kNavy, 50)) : kWhite;
  triangle(20.0f, 7.0f, 5.5f, ground);
  triangle(19.0f, 8.0f, 4.5f, outline);
  triangle(18.0f, 9.0f, 3.5f, to565(fill));
}

void draw_time(Canvas& c, const ViewModel& view, int32_t baseline) {
  if (!view.time_valid) {
    c.text(kNoTime, kCenter, baseline, &kInterTime, kWhite, lgfx::textdatum_t::baseline_center);
    return;
  }
  // Hours and minutes are placed around a fixed colon, so the blink never shifts them.
  char hours[4];
  char minutes[4];
  char colon[2];
  flash_format(hours, sizeof(hours), kTwoDigits, view.local.hour);
  flash_format(minutes, sizeof(minutes), kTwoDigits, view.local.minute);
  flash_copy(colon, sizeof(colon), kColon);
  c.g.setFont(&kInterTime);
  const int32_t half_colon = c.g.textWidth(colon) / 2 + 2;
  c.ram_text(hours, kCenter - half_colon, baseline, &kInterTime, kWhite, lgfx::textdatum_t::baseline_right);
  c.ram_text(minutes, kCenter + half_colon, baseline, &kInterTime, kWhite, lgfx::textdatum_t::baseline_left);
  if (view.local.second % 2 == 0) {
    c.ram_text(colon, kCenter, baseline - 4, &kInterTime, kWhite, lgfx::textdatum_t::baseline_center);
  }
}

// The card under the clock: what NEPTUN shows near home.
void draw_threat_card(Canvas& c, const ViewModel& view, const Snapshot& s, int32_t top) {
  constexpr int32_t kLeft = 14;
  constexpr int32_t kWidth = kSize - 2 * kLeft;
  constexpr int32_t kHeight = 48;
  if (!c.touches(top, kHeight)) {
    return;
  }
  char title[64];
  char detail[64];
  char age[16];
  Rgb badge = kGreyRgb;
  uint16_t title_color = kWhite;
  int16_t heading = -1;
  bool nearest = false;
  if (!view.threats_fresh) {
    format_age(age, sizeof(age), view.threats_age_s);
    flash_format(title, sizeof(title), kCardStaleTitle, age);
    flash_copy(detail, sizeof(detail), kCardStaleDetail);
    badge = kOrangeRgb;
    title_color = kOrange;
  } else if (s.area_only_in_region > 0) {
    flash_format(title, sizeof(title), kCardRegionTitle, static_cast<unsigned>(s.area_only_in_region));
    flash_copy(detail, sizeof(detail), kCardRegionDetail);
    badge = kAmberRgb;
  } else if (s.nearest_km >= 0.0f) {
    const int km = static_cast<int>(std::lround(s.nearest_km));
    if (s.threats_in_watch > 0) {
      flash_format(title, sizeof(title), kCardNearTitle, static_cast<unsigned>(s.threats_in_watch));
    } else {
      flash_copy(title, sizeof(title), kCardFarTitle);
    }
    // Advisories are observations, not shelter signals: hollow, and named as such.
    flash_format(detail, sizeof(detail), s.nearest_advisory ? kCardNearestAdvisory : kCardNearestTrack, km);
    badge = marker_rgb(s.nearest_kind, s.nearest_advisory, s.nearest_stale, kCardRgb);
    heading = s.nearest_heading_deg;
    nearest = true;
  } else {
    format_age(age, sizeof(age), view.threats_age_s);
    flash_copy(title, sizeof(title), kCardClearTitle);
    flash_format(detail, sizeof(detail), kCardClearDetail, age);
    title_color = kMuted;
  }

  c.g.fillRoundRect(kLeft, c.y(top), kWidth, kHeight, 10, kCard);
  c.g.drawRoundRect(kLeft, c.y(top), kWidth, kHeight, 10, kLine);
  if (nearest) {
    draw_threat_badge(c, kLeft + 22, top + kHeight / 2, s.nearest_kind, heading, badge,
                       s.nearest_advisory, s.nearest_stale, kCard);
  } else {
    draw_status_badge(c, kLeft + 22, top + kHeight / 2, badge, kCard);
  }
  c.ram_text(title, kLeft + 46, top + 22, &kInterTitle, title_color, lgfx::textdatum_t::baseline_left);
  c.ram_text(detail, kLeft + 46, top + 39, &kInterBody, kMuted, lgfx::textdatum_t::baseline_left);
}

void draw_credit(Canvas& c) {
  constexpr int32_t baseline = kSize - 6;
  if (c.touches(baseline - 10, 13)) {
    c.text(kCredit, kCenter, baseline, &kInterSmall, kMuted, lgfx::textdatum_t::baseline_center);
  }
}

void draw_clock_panel(Canvas& c, const ViewModel& view, const Snapshot& s) {
  c.g.fillScreen(kBackground);
  draw_pill(c, kCenter, 12, view.banner, PillLabelStyle::Full, HorizontalAnchor::Center,
            view.alert_duration_s, kSize - 12);

  const int32_t time_baseline = 110;
  if (c.touches(time_baseline - 56, 60)) {
    draw_time(c, view, time_baseline);
  }

  const int16_t date_y = 140;
  if (c.touches(date_y - 12, 24)) {
    char text[64];
    if (view.time_valid) {
      char weekday[kNameBytes];
      char month[kNameBytes];
      flash_copy(weekday, sizeof(weekday), kWeekdays[view.local.weekday % 7]);
      flash_copy(month, sizeof(month), kMonths[(view.local.month + 11) % 12]);
      flash_format(text, sizeof(text), kDateFormat, weekday, view.local.day, month);
    } else {
      flash_copy(text, sizeof(text), kWaitingForTime);
    }
    c.ram_text(text, kCenter, date_y, &kInterBody, kMuted, lgfx::textdatum_t::middle_center);
  }

  draw_threat_card(c, view, s, 158);
}

// Coloured squares with a count per kind, most dangerous first, as many as fit.
void draw_legend(Canvas& c, const Snapshot& s, int32_t baseline, int32_t right) {
  uint16_t counts[kKinds] = {};
  for (const Marker& marker : s.markers) {
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
  int32_t x = 6;
  c.g.setFont(&kInterSmall);
  for (size_t i = 0; i < kKinds; ++i) {
    if (counts[i] == 0) {
      continue;
    }
    char name[kNameBytes];
    char text[32];
    flash_copy(name, sizeof(name), kKindLabels[static_cast<size_t>(kLegendOrder[i]) % kKinds]);
    flash_format(text, sizeof(text), kLegendFormat, name, static_cast<unsigned>(counts[i]));
    c.g.setFont(&kInterSmall);
    const int32_t width = 11 + c.g.textWidth(text);
    if (x + width > right) {
      break;
    }
    c.g.fillRoundRect(x, c.y(baseline - 7), 7, 7, 2, to565(kind_rgb(kLegendOrder[i])));
    c.ram_text(text, x + 11, baseline, &kInterSmall, kWhite, lgfx::textdatum_t::baseline_left);
    x += width + 8;
  }
}

// Fills the strip from the basemap's runs; false when there is no basemap to draw.
bool draw_basemap(Canvas& c, const core::Basemap* basemap) {
  if (basemap == nullptr || !basemap->valid()) {
    return false;
  }
  for (int32_t row = 0; row < c.g.height(); ++row) {
    const int16_t y = static_cast<int16_t>(c.top + row);
    basemap->for_each_run(y, [&](int16_t x, int16_t length, uint16_t color) {
      c.g.drawFastHLine(x, row, length, color);
    });
  }
  return true;
}

bool clip_track_axis(float start, float delta, float low, float high, float& first, float& last) {
  if (delta == 0.0f) {
    return start >= low && start <= high;
  }
  const float a = (low - start) / delta;
  const float b = (high - start) / delta;
  first = std::fmax(first, std::fmin(a, b));
  last = std::fmin(last, std::fmax(a, b));
  return first <= last;
}

void draw_track_line(Canvas& c, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     uint16_t color) {
  const float delta_x = static_cast<float>(x1) - x0;
  const float delta_y = static_cast<float>(y1) - y0;
  float first = 0.0f;
  float last = 1.0f;
  if (!clip_track_axis(x0, delta_x, -2, kSize + 1, first, last) ||
      !clip_track_axis(y0, delta_y, -2, kSize + 1, first, last)) {
    return;
  }
  // Bound raster work to the screen; every strip uses the same clipped endpoints.
  x1 = static_cast<int32_t>(std::lround(x0 + delta_x * last));
  y1 = static_cast<int32_t>(std::lround(y0 + delta_y * last));
  x0 = static_cast<int32_t>(std::lround(x0 + delta_x * first));
  y0 = static_cast<int32_t>(std::lround(y0 + delta_y * first));
  const int32_t top = y0 < y1 ? y0 : y1;
  const int32_t bottom = y0 > y1 ? y0 : y1;
  if (!c.touches(top - 1, bottom - top + 4) || (x0 == x1 && y0 == y1)) {
    return;
  }
  const int32_t dx = x0 < x1 ? x1 - x0 : x0 - x1;
  const int32_t dy = y0 < y1 ? y1 - y0 : y0 - y1;
  for (int offset = -1; offset <= 2; ++offset) {
    const int32_t ox = dy > dx ? offset : 0;
    const int32_t oy = dy > dx ? 0 : offset;
    c.g.drawLine(x0 + ox, c.y(y0 + oy), x1 + ox, c.y(y1 + oy), kBackground);
  }
  for (int offset = 0; offset <= 1; ++offset) {
    const int32_t ox = dy > dx ? offset : 0;
    const int32_t oy = dy > dx ? 0 : offset;
    c.g.drawLine(x0 + ox, c.y(y0 + oy), x1 + ox, c.y(y1 + oy), color);
  }
}

void draw_prediction(Canvas& c, const Marker& marker, Rgb color) {
  const float dx = static_cast<float>(marker.point.x) - marker.anchor.x;
  const float dy = static_cast<float>(marker.point.y) - marker.anchor.y;
  if (!marker.predicted || (dx == 0.0f && dy == 0.0f)) {
    return;
  }
  float first = 0.0f;
  float last = 1.0f;
  if (!clip_track_axis(marker.anchor.x, dx, -2, kSize + 1, first, last) ||
      !clip_track_axis(marker.anchor.y, dy, c.top - 2, c.top + c.g.height() + 1, first, last)) {
    return;
  }
  const float length = std::sqrt(dx * dx + dy * dy);
  // Dash positions use the original anchor, so clipping each strip preserves their phase.
  const int32_t first_dash = static_cast<int32_t>(std::floor(first * length / 9.0f));
  const int32_t last_dash = static_cast<int32_t>(std::floor(last * length / 9.0f));
  for (int32_t i = first_dash; i <= last_dash; ++i) {
    const float begin = static_cast<float>(i) * 9.0f / length;
    const float end = std::fmin(1.0f, (static_cast<float>(i) * 9.0f + 4.0f) / length);
    draw_track_line(c, static_cast<int32_t>(std::lround(marker.anchor.x + dx * begin)),
                    static_cast<int32_t>(std::lround(marker.anchor.y + dy * begin)),
                    static_cast<int32_t>(std::lround(marker.anchor.x + dx * end)),
                    static_cast<int32_t>(std::lround(marker.anchor.y + dy * end)),
                    to565(mix(kNavy, color, 75)));
  }
}

void draw_trail(Canvas& c, const Marker& marker, Rgb color) {
  const size_t count = marker.trail_count < core::kMaxTrail ? marker.trail_count : core::kMaxTrail;
  for (size_t i = 0; i < count; ++i) {
    const core::ScreenPoint from = marker.trail[i];
    const core::ScreenPoint to = i + 1 < count ? marker.trail[i + 1]
                                             : (marker.predicted ? marker.anchor : marker.point);
    const int brightness = 40 + static_cast<int>(60 * (i + 1) / count);
    draw_track_line(c, from.x, from.y, to.x, to.y,
                    to565(mix(kNavy, mix(color, kWhiteRgb, 20), brightness)));
  }
  draw_prediction(c, marker, color);
}

void draw_nearest_focus(Canvas& c, const Marker& marker) {
  constexpr int32_t kRadius = 12;
  constexpr int32_t kArm = 4;
  const int32_t x = marker.point.x;
  const int32_t y = marker.point.y;
  if (!c.touches(y - kRadius - 1, 2 * kRadius + 3)) {
    return;
  }
  const uint16_t ink = marker.stale ? to565(mix(kWhiteRgb, kNavy, 50)) : kWhite;
  for (int32_t sx = -1; sx <= 1; sx += 2) {
    for (int32_t sy = -1; sy <= 1; sy += 2) {
      const int32_t cx = x + sx * kRadius;
      const int32_t cy = y + sy * kRadius;
      const int32_t left = sx < 0 ? cx : cx - kArm;
      const int32_t top = sy < 0 ? cy : cy - kArm;
      c.g.fillRect(left - 1, c.y(cy - 1), kArm + 3, 3, kBackground);
      c.g.fillRect(cx - 1, c.y(top - 1), 3, kArm + 3, kBackground);
      c.g.drawFastHLine(left, c.y(cy), kArm + 1, ink);
      c.g.drawFastVLine(cx, c.y(top), kArm + 1, ink);
    }
  }
}

void draw_map_footer(Canvas& c, const ViewModel& view, const Snapshot& s) {
  const int16_t footer_y = kSize - kFooterHeight;
  if (!c.touches(footer_y, kFooterHeight)) {
    return;
  }
  c.g.fillRect(0, c.y(footer_y), kSize, kFooterHeight, kBackground);
  c.g.drawFastHLine(0, c.y(footer_y), kSize, kLine);
  const int32_t first_baseline = footer_y + 13;
  if (!view.threats_fresh) {
    char age[16];
    char text[40];
    format_age(age, sizeof(age), view.threats_age_s);
    flash_format(text, sizeof(text), kMapStale, age);
    c.ram_text(text, 6, first_baseline, &kInterSmall, kOrange, lgfx::textdatum_t::baseline_left);
  } else {
    draw_legend(c, s, first_baseline, kSize - 6);
  }
}

void draw_map_panel(Canvas& c, const ViewModel& view, const Snapshot& s) {
  if (!draw_basemap(c, view.basemap)) {
    c.g.fillScreen(kBackground);
  }
  const Rgb ground_rgb = kNavy;

  for (const uint8_t km : s.ring_km) {
    const int32_t radius = static_cast<int32_t>(std::lround(static_cast<float>(km) * s.px_per_km));
    c.g.drawCircle(kCenter, c.y(kCenter), radius, kRing);
    char label[12];
    flash_format(label, sizeof(label), kRingFormat, static_cast<unsigned>(km));
    if (c.touches(kCenter - radius - 12, 12)) {
      c.ram_text(label, kCenter, kCenter - radius - 1, &kInterSmall, kMuted, lgfx::textdatum_t::baseline_center);
    }
  }

  // Uncertainty halos and trails first, so neither covers a badge.
  for (const Marker& marker : s.markers) {
    const Rgb color = marker_rgb(marker.kind, marker.advisory, marker.stale, ground_rgb);
    const int32_t r = marker.uncertainty_px;
    if (r > kBadgeRadius && c.touches(marker.point.y - r, 2 * r + 1)) {
      c.g.drawCircle(marker.point.x, c.y(marker.point.y), r, to565(mix(ground_rgb, color, 25)));
    }
    draw_trail(c, marker, color);
  }

  if (c.touches(kCenter - 6, 13)) {
    c.g.drawCircle(kCenter, c.y(kCenter), 5, kWhite);
    c.g.fillCircle(kCenter, c.y(kCenter), 1, kWhite);
  }

  // Farthest first, so the nearest track is drawn on top.
  for (size_t i = s.markers.size(); i-- > 0;) {
    const Marker& marker = s.markers[i];
    draw_threat_badge(c, marker.point.x, marker.point.y, marker.kind, marker.heading_deg,
                      marker_rgb(marker.kind, marker.advisory, marker.stale, ground_rgb),
                      marker.advisory, marker.stale, kBackground);
  }

  // Gateway markers are nearest first; focus the first centre inside the map viewport.
  for (const Marker& marker : s.markers) {
    if (marker.point.x >= 0 && marker.point.x < kSize && marker.point.y >= 0 &&
        marker.point.y < kSize - kFooterHeight) {
      draw_nearest_focus(c, marker);
      break;
    }
  }

  // Top: the banner pill, the time pill and north.
  const int32_t pill_top = 6;
  if (c.touches(pill_top, kPillHeight)) {
    char text[8];
    if (view.time_valid) {
      flash_format(text, sizeof(text), kTimeFormat, view.local.hour, view.local.minute);
    } else {
      flash_copy(text, sizeof(text), kNoTime);
    }
    c.g.setFont(&kInterTitle);
    const int32_t width = c.g.textWidth(text) + 16;
    if (view.banner != Banner::None) {
      draw_pill(c, 6, pill_top, view.banner, PillLabelStyle::Compact, HorizontalAnchor::Left,
                view.alert_duration_s, kSize - 6 - width - 6 - 4);
    }
    c.g.fillRoundRect(kSize - 6 - width, c.y(pill_top), width, kPillHeight, kPillHeight / 2, kBackground);
    c.ram_text(text, kSize - 6 - width / 2, pill_top + kPillHeight / 2, &kInterTitle, kWhite,
               lgfx::textdatum_t::middle_center);
    if (view.banner == Banner::None) {
      c.text(kNorth, 8, pill_top + kPillHeight / 2, &kInterSmall, kMuted, lgfx::textdatum_t::middle_left);
    }
  }

  draw_map_footer(c, view, s);
}

void draw_notice(Canvas& c) {
  c.g.fillScreen(kBackground);
  for (size_t i = 0; i < 4; ++i) {
    const int32_t baseline = 92 + 22 * static_cast<int32_t>(i);
    if (c.touches(baseline - 16, 20)) {
      c.text(kNoticeLines[i], kCenter, baseline, &kInterTitle, kWhite, lgfx::textdatum_t::baseline_center);
    }
  }
}

}  // namespace

void draw_scene(lgfx::LovyanGFX& g, int16_t top, const ViewModel& view) {
  Canvas canvas{g, top};
  if (view.snapshot == nullptr) {  // a ViewModel that App::build_view did not fill
    g.fillScreen(kBackground);
    return;
  }
  const Snapshot& snapshot = *view.snapshot;
  if (view.notice) {
    draw_notice(canvas);
  } else if (view.panel == Panel::Map) {
    draw_map_panel(canvas, view, snapshot);
  } else {
    draw_clock_panel(canvas, view, snapshot);
  }
  draw_credit(canvas);
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
