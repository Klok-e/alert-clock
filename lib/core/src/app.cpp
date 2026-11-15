#include "app.h"

#include <cmath>

namespace core {
namespace {

constexpr size_t kMaxHeadBytes = 16 * 1024;     // Cloudflare sends ~1 KB of headers
constexpr size_t kMaxThreatBytes = 1536 * 1024;  // a mass-attack snapshot is a few hundred KB
constexpr size_t kMaxAlertBytes = 64;
constexpr uint32_t kMaxBackoffMs = 120000;
constexpr uint32_t kRateLimitedBackoffMs = 60000;
// The APIs' limits: NEPTUN asks for at most one poll every 5 s; alerts.in.ua allows 8-10
// requests a minute per IP and blocks tokens that keep exceeding it.
constexpr uint32_t kMinThreatsIntervalMs = 5000;
constexpr uint32_t kMinAlertsIntervalMs = 7500;
// Sent instead of the real token when the alerts URL is plain HTTP (the LAN mock server).
constexpr const char* kPlaceholderToken = "sent-only-over-tls";
constexpr int16_t kOffscreenMarginPx = 24;
constexpr float kMaxUncertaintyPx = 200.0f;

void choose_rings(float radius_km, etl::ivector<uint8_t>& rings) {
  static constexpr uint8_t kSteps[] = {1, 2, 5, 10, 15, 20, 25, 50, 100};
  const float target = radius_km / 3.0f;
  uint8_t step = kSteps[0];
  for (const uint8_t candidate : kSteps) {
    if (static_cast<float>(candidate) <= target) {
      step = candidate;
    }
  }
  for (int i = 1; i <= static_cast<int>(kMaxRings); ++i) {
    const int km = step * i;
    if (static_cast<float>(km) > radius_km + 0.01f || km > 255) {
      break;
    }
    rings.push_back(static_cast<uint8_t>(km));
  }
}

// Saturates below kNever, which means "no data yet".
uint32_t age_s(uint64_t now_ms, uint64_t then_ms) {
  const uint64_t seconds = (now_ms - then_ms) / 1000;
  return seconds < kNever ? static_cast<uint32_t>(seconds) : kNever - 1;
}

bool source_from(const char* text, Url& url) { return text != nullptr && parse_url(text, url); }

}  // namespace

const char* sound_name(Sound sound) {
  switch (sound) {
    case Sound::None:
      return "none";
    case Sound::AlertRaised:
      return "raised";
    case Sound::AlertEscalated:
      return "escalated";
    case Sound::AlertCleared:
      return "cleared";
  }
  return "none";
}

const char* fetch_outcome_name(FetchOutcome outcome) {
  switch (outcome) {
    case FetchOutcome::None:
      return "none";
    case FetchOutcome::Ok:
      return "ok";
    case FetchOutcome::NotModified:
      return "not_modified";
    case FetchOutcome::ConnectFailed:
      return "connect_failed";
    case FetchOutcome::RequestFailed:
      return "request_failed";
    case FetchOutcome::BadHead:
      return "bad_head";
    case FetchOutcome::HttpError:
      return "http_error";
    case FetchOutcome::ParseFailed:
      return "parse_failed";
  }
  return "none";
}

App::App(const Settings& settings, TimeSource& time, Transport& transport)
    : settings_(settings), time_(time), transport_(transport), uptime_millis_(time.millis()) {
  if (settings_.threats_period_ms < kMinThreatsIntervalMs) {
    settings_.threats_period_ms = kMinThreatsIntervalMs;
  }
  if (settings_.alerts_period_ms < kMinAlertsIntervalMs) {
    settings_.alerts_period_ms = kMinAlertsIntervalMs;
  }
  threats_.min_interval_ms = kMinThreatsIntervalMs;
  alerts_.min_interval_ms = kMinAlertsIntervalMs;
  threats_.configured = source_from(settings_.threats_url, threats_.url);
  alerts_.configured = settings_.alerts_token != nullptr && *settings_.alerts_token != '\0' &&
                       source_from(settings_.alerts_url, alerts_.url);
}

bool App::set_urls(const char* threats_url, const char* alerts_url) {
  Url threats;
  Url alerts;
  if ((threats_url != nullptr && !parse_url(threats_url, threats)) ||
      (alerts_url != nullptr && !parse_url(alerts_url, alerts))) {
    return false;
  }
  if (threats_url != nullptr) {
    threats_.url = threats;
    threats_.configured = true;
    threats_.etag.clear();
    threats_.last_modified.clear();
  }
  if (alerts_url != nullptr) {
    alerts_.url = alerts;
    alerts_.configured = settings_.alerts_token != nullptr && *settings_.alerts_token != '\0';
    alerts_.etag.clear();
    alerts_.last_modified.clear();
  }
  refresh_now();
  return true;
}

void App::refresh_now() {
  threats_.forced = true;
  alerts_.forced = true;
}

void App::press(ButtonEvent event) {
  if (event == ButtonEvent::Short) {
    map_requested_ = !map_requested_;
    map_requested_at_ = time_.millis();
  } else if (event == ButtonEvent::Long) {
    refresh_now();
  }
}

Sound App::take_sound() {
  const Sound sound = pending_sound_;
  pending_sound_ = Sound::None;
  return sound;
}

bool App::due(const Source& source, uint32_t now) const {
  if (!source.configured) {
    return false;
  }
  if (!source.requested || static_cast<int32_t>(now - source.status.next_due_ms) >= 0) {
    return true;
  }
  return source.forced && source.status.last_http_status != 429 &&
         now - source.last_request_ms >= source.min_interval_ms;
}

bool App::fresh(const Source& source, uint32_t now) const {
  return source.status.ever_succeeded && !source.status.expired &&
         now - source.status.last_success_ms < settings_.fresh_for_ms;
}

// Latching keeps the uint32 age from wrapping back into "fresh" after ~49.7 days of failures.
void App::expire_if_old(Source& source, uint32_t now) {
  SourceStatus& status = source.status;
  if (status.ever_succeeded && !status.expired && now - status.last_success_ms >= settings_.fresh_for_ms) {
    status.expired = true;
  }
}

uint64_t App::advance_uptime() {
  const uint32_t now = time_.millis();
  uptime_ms_ += now - uptime_millis_;
  uptime_millis_ = now;
  return uptime_ms_;
}

void App::tick() {
  advance_uptime();
  const uint32_t now = time_.millis();
  if (map_requested_ && now - map_requested_at_ >= settings_.map_request_ms) {
    map_requested_ = false;
  }
  expire_if_old(alerts_, now);
  expire_if_old(threats_, now);
  if (time_.unix_time() == 0) {
    return;  // TLS certificate checks need the time, and so do track ages
  }
  if (due(alerts_, now)) {
    record(alerts_, fetch(alerts_, false), settings_.alerts_period_ms);
  } else if (due(threats_, now)) {
    record(threats_, fetch(threats_, true), settings_.threats_period_ms);
  }
}

void App::record(Source& source, FetchOutcome outcome, uint32_t period_ms) {
  const uint32_t now = time_.millis();
  SourceStatus& status = source.status;
  source.forced = false;
  status.last_outcome = outcome;
  if (outcome == FetchOutcome::Ok || outcome == FetchOutcome::NotModified) {
    ++status.successes;
    status.consecutive_failures = 0;
    status.ever_succeeded = true;
    status.expired = false;
    status.last_success_ms = now;
    source.success_uptime_ms = advance_uptime();
    status.next_due_ms = now + period_ms;
    return;
  }
  ++status.failures;
  if (status.consecutive_failures < UINT8_MAX) {
    ++status.consecutive_failures;
  }
  const uint8_t shift = status.consecutive_failures < 3 ? status.consecutive_failures : 3;
  uint32_t delay = period_ms << shift;
  if (delay > kMaxBackoffMs) {
    delay = kMaxBackoffMs;
  }
  if (status.last_http_status == 429 && delay < kRateLimitedBackoffMs) {
    delay = kRateLimitedBackoffMs;
  }
  status.next_due_ms = now + delay;
}

FetchOutcome App::fetch(Source& source, bool threats) {
  source.requested = true;
  source.last_request_ms = time_.millis();
  source.status.last_http_status = 0;
  RequestOptions options;
  if (!threats) {
    options.bearer_token = source.url.tls ? settings_.alerts_token : kPlaceholderToken;
  }
  options.if_none_match = source.etag.empty() ? nullptr : source.etag.c_str();
  options.if_modified_since = source.last_modified.empty() ? nullptr : source.last_modified.c_str();
  if (!build_get_request(source.url, options, request_)) {
    return FetchOutcome::RequestFailed;
  }
  if (!transport_.connect(source.url)) {
    transport_.close();
    return FetchOutcome::ConnectFailed;
  }
  FetchOutcome outcome = FetchOutcome::Ok;
  if (!transport_.write(request_.data(), request_.size())) {
    outcome = FetchOutcome::RequestFailed;
  } else {
    LimitedSource head_input(transport_.input(), kMaxHeadBytes);
    ResponseHead head;
    if (read_response_head(head_input, head) != HeadResult::Ok) {
      outcome = FetchOutcome::BadHead;
    } else {
      source.status.last_http_status = head.status;
      if (head.status == 304) {
        outcome = FetchOutcome::NotModified;
      } else if (head.status != 200) {
        outcome = FetchOutcome::HttpError;
      } else {
        const size_t limit = threats ? kMaxThreatBytes : kMaxAlertBytes;
        const bool sized = head.content_length >= 0;
        if (sized && static_cast<size_t>(head.content_length) > limit) {
          outcome = FetchOutcome::ParseFailed;  // more than this source ever needs
        } else {
          // A known length also ends the read without waiting for the connection to close.
          LimitedSource body(transport_.input(), sized ? static_cast<size_t>(head.content_length) : limit);
          outcome = threats ? read_threats(body) : read_alert(body);
          if (outcome == FetchOutcome::Ok) {
            source.etag = head.etag;
            source.last_modified = head.last_modified;
          }
        }
      }
    }
  }
  transport_.close();
  return outcome;
}

FetchOutcome App::read_alert(ByteSource& body) {
  char text[kMaxAlertBytes];
  const size_t length = body.readBytes(text, sizeof(text));
  if (length == sizeof(text)) {
    return FetchOutcome::ParseFailed;  // a status letter never needs this much
  }
  const AlertLevel level = parse_iot_alert(text, length);
  if (level == AlertLevel::Unknown) {
    return FetchOutcome::ParseFailed;
  }
  alert_level_ = level;
  switch (tracker_.update(level)) {
    case AlertChange::Raised:
      pending_sound_ = Sound::AlertRaised;
      break;
    case AlertChange::Escalated:
      pending_sound_ = Sound::AlertEscalated;
      break;
    case AlertChange::Cleared:
      pending_sound_ = Sound::AlertCleared;
      break;
    case AlertChange::None:
      break;
  }
  return FetchOutcome::Ok;
}

FetchOutcome App::read_threats(ByteSource& body) {
  ThreatQuery query;
  query.home = settings_.home;
  query.radius_km = settings_.map_radius_km * 1.5f;  // keep tracks just off the map edge
  query.home_region = settings_.home_region;
  last_parse_ = parse_neptun_threats(body, query, incoming_);
  if (last_parse_ != ThreatParse::Ok) {
    return FetchOutcome::ParseFailed;
  }
  snapshot_ = incoming_;
  return FetchOutcome::Ok;
}

void App::build_view(ViewModel& view) {
  const uint32_t now_ms = time_.millis();
  const int64_t now = time_.unix_time();
  view = ViewModel{};

  Situation situation;
  situation.alert = alert_level_;
  situation.alert_fresh = fresh(alerts_, now_ms);
  situation.threats_fresh = fresh(threats_, now_ms);
  situation.map_requested = map_requested_;
  for (const Threat& threat : snapshot_.nearby) {
    if (!threat.advisory && threat.distance_km <= settings_.watch_radius_km) {
      ++situation.threats_in_watch;
    }
  }
  const ScreenPlan plan = plan_screen(situation);
  view.panel = plan.panel;
  view.banner = plan.banner;
  view.threats_fresh = situation.threats_fresh;
  view.threats_in_watch = situation.threats_in_watch;
  view.area_only_in_region = snapshot_.area_only_in_region;
  view.nearest_km = snapshot_.nearby.empty() ? -1.0f : snapshot_.nearby.front().distance_km;
  const uint64_t uptime = advance_uptime();
  if (threats_.status.ever_succeeded) {
    view.threats_age_s = age_s(uptime, threats_.success_uptime_ms);
  }
  if (alerts_.status.ever_succeeded) {
    view.alert_age_s = age_s(uptime, alerts_.success_uptime_ms);
  }
  if (now > 0) {
    view.time_valid = time_.to_local(now, view.local);
  }

  const LocalProjection projection(settings_.home, settings_.map_radius_km, kScreenPx);
  view.px_per_km = projection.px_per_km();
  choose_rings(settings_.map_radius_km, view.ring_km);
  for (const Threat& threat : snapshot_.nearby) {
    const LatLon position = now > 0 ? predicted_position(threat, now) : threat.position;
    ThreatMarker marker;
    marker.point = projection.project(position);
    if (marker.point.x < -kOffscreenMarginPx || marker.point.x > kScreenPx + kOffscreenMarginPx ||
        marker.point.y < -kOffscreenMarginPx || marker.point.y > kScreenPx + kOffscreenMarginPx) {
      continue;
    }
    if (threat.approximate && threat.uncertainty_km > 0.0f) {
      const float px = threat.uncertainty_km * view.px_per_km;
      marker.uncertainty_px = static_cast<int16_t>(px > kMaxUncertaintyPx ? kMaxUncertaintyPx : px);
    }
    marker.heading_deg = threat.heading_deg;
    marker.kind = threat.kind;
    marker.approximate = threat.approximate;
    marker.stale = threat.stale;
    marker.advisory = threat.advisory;
    view.markers.push_back(marker);
  }
}

}  // namespace core
