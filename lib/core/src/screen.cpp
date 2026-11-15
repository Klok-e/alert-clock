#include "screen.h"

namespace core {

const char* panel_name(Panel panel) { return panel == Panel::Map ? "map" : "clock"; }

const char* banner_name(Banner banner) {
  switch (banner) {
    case Banner::None:
      return "none";
    case Banner::NoData:
      return "nodata";
    case Banner::Partial:
      return "partial";
    case Banner::Yellow:
      return "yellow";
    case Banner::Red:
      return "red";
  }
  return "nodata";
}

ScreenPlan plan_screen(const Situation& situation) {
  ScreenPlan plan;
  const bool threats_near = situation.threats_fresh && situation.threats_in_watch > 0;
  plan.panel = (situation.map_requested || threats_near) ? Panel::Map : Panel::Clock;
  if (!situation.alert_fresh) {
    plan.banner = Banner::NoData;
    return plan;
  }
  switch (situation.alert) {
    case AlertLevel::None:
      plan.banner = Banner::None;
      break;
    case AlertLevel::Partial:
      plan.banner = Banner::Partial;
      break;
    case AlertLevel::Yellow:
      plan.banner = Banner::Yellow;
      break;
    case AlertLevel::Red:
      plan.banner = Banner::Red;
      break;
    case AlertLevel::Unknown:
      plan.banner = Banner::NoData;
      break;
  }
  return plan;
}

}  // namespace core
