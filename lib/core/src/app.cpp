#include "app.h"

#include <cstdio>
#include <cstring>

#include <etl/string.h>

namespace core {
namespace {

constexpr size_t kMaxHeadBytes = 4096;
constexpr const char* kDefaultPort = "8080";

// Saturates below kNever, which means "no data yet".
uint32_t age_s(uint64_t now_ms, uint64_t then_ms) {
  const uint64_t seconds = (now_ms - then_ms) / 1000;
  return seconds < kNever ? static_cast<uint32_t>(seconds) : kNever - 1;
}

uint32_t add_saturating(uint32_t a, uint32_t b) { return a > kNever - 1 - b ? kNever - 1 : a + b; }

}  // namespace

const char* read_end_name(ReadEnd reason) {
  switch (reason) {
    case ReadEnd::None: return "none";
    case ReadEnd::Closed: return "closed";
    case ReadEnd::Timeout: return "timeout";
    case ReadEnd::Error: return "error";
  }
  return "error";
}

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
    case FetchOutcome::NotConfigured:
      return "not_configured";
    case FetchOutcome::ConnectFailed:
      return "connect_failed";
    case FetchOutcome::RequestFailed:
      return "request_failed";
    case FetchOutcome::BadHead:
      return "bad_head";
    case FetchOutcome::HttpError:
      return "http_error";
    case FetchOutcome::TooLarge:
      return "too_large";
    case FetchOutcome::Truncated:
      return "truncated";
    case FetchOutcome::Rejected:
      return "rejected";
  }
  return "none";
}

App::App(const Settings& settings, TimeSource& time, Transport& transport, Random& random)
    : settings_(settings),
      time_(time),
      transport_(transport),
      random_(random),
      uptime_millis_(time.millis()),
      reply_{},
      basemap_body_{} {}

bool App::set_gateway(const char* address) {
  if (address == nullptr || *address == '\0' || std::strchr(address, '/') != nullptr) {
    return false;
  }
  const bool has_port = std::strchr(address, ':') != nullptr;
  etl::string<96> text;
  text.assign("http://");
  text.append(address);
  if (!has_port) {
    text.append(":");
    text.append(kDefaultPort);
  }
  if (text.full()) {
    return false;
  }
  etl::string<96> basemap_text = text;
  text.append("/v1/snapshot");
  basemap_text.append("/v1/basemap");
  Url snapshot;
  Url basemap;
  if (text.full() || basemap_text.full() || !parse_url(text.c_str(), snapshot) ||
      !parse_url(basemap_text.c_str(), basemap)) {
    return false;
  }
  snapshot_url_ = snapshot;
  basemap_url_ = basemap;
  configured_ = true;
  basemap_.invalidate();
  refresh_now();
  return true;
}

void App::refresh_now() { forced_ = true; }

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

uint64_t App::advance_uptime() {
  const uint32_t now = time_.millis();
  uptime_ms_ += now - uptime_millis_;
  uptime_millis_ = now;
  return uptime_ms_;
}

uint32_t App::since_valid_s() {
  return status_.ever_valid ? age_s(advance_uptime(), valid_uptime_ms_) : kNever;
}

bool App::snapshot_recent() const {
  return status_.ever_valid && uptime_ms_ - valid_uptime_ms_ < settings_.silent_ms;
}

void App::tick() {
  advance_uptime();
  const uint32_t now = time_.millis();
  if (map_requested_ && now - map_requested_at_ >= settings_.map_request_ms) {
    map_requested_ = false;
  }
  if (!configured_) {
    status_.last_outcome = FetchOutcome::NotConfigured;
    return;
  }
  if (basemap_wanted_) {
    basemap_wanted_ = false;
    basemap_failed_ = fetch(Request::Basemap) != FetchOutcome::Ok;
    basemap_failed_at_ = now;
    return;
  }
  const uint32_t interval = last_poll_failed_ ? settings_.retry_ms : settings_.poll_ms;
  if (forced_ || !requested_ || now - last_request_ms_ >= interval) {
    forced_ = false;
    requested_ = true;
    last_request_ms_ = now;
    const FetchOutcome outcome = fetch(Request::Snapshot);
    status_.last_outcome = outcome;
    last_poll_failed_ = outcome != FetchOutcome::Ok;
    if (outcome == FetchOutcome::Ok) {
      ++status_.successes;
      accept_snapshot();
    } else {
      ++status_.failures;
      if (outcome == FetchOutcome::Rejected) {
        ++status_.rejections;
      }
    }
  }
}

void App::accept_snapshot() {
  status_.ever_valid = true;
  valid_uptime_ms_ = advance_uptime();
  switch (tracker_.update(snapshot_.alert_level)) {
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
  if (snapshot_.basemap_version == 0) {
    basemap_.invalidate();
  } else if (basemap_.version() != snapshot_.basemap_version &&
             (!basemap_failed_ || time_.millis() - basemap_failed_at_ >= settings_.basemap_retry_ms)) {
    basemap_wanted_ = true;
  }
}

FetchOutcome App::fetch(Request request) {
  const uint32_t started = time_.millis();
  TransferStatus transfer;
  transfer.outcome = fetch_body(request, transfer);
  transfer.duration_ms = time_.millis() - started;
  if (request == Request::Basemap) {
    basemap_status_.last = transfer;
    if (transfer.outcome == FetchOutcome::Ok) {
      ++basemap_status_.successes;
    } else {
      ++basemap_status_.failures;
      basemap_status_.last_failure = transfer;
    }
  }
  return transfer.outcome;
}

FetchOutcome App::fetch_body(Request request, TransferStatus& transfer) {
  const bool snapshot = request == Request::Snapshot;
  const Url& url = snapshot ? snapshot_url_ : basemap_url_;
  uint8_t* buffer = snapshot ? reply_ : basemap_body_;
  const size_t capacity = snapshot ? sizeof(reply_) : sizeof(basemap_body_);
  if (!snapshot) {
    basemap_.invalidate();  // the download overwrites the buffer the map is drawn from
  }

  const uint32_t nonce = random_.next();
  char query[16];
  std::snprintf(query, sizeof(query), "n=%08lx", static_cast<unsigned long>(nonce));
  RequestOptions options;
  options.query = query;
  if (!build_get_request(url, options, request_)) {
    return FetchOutcome::RequestFailed;
  }
  if (!transport_.connect(url)) {
    transport_.close();
    return FetchOutcome::ConnectFailed;
  }
  FetchOutcome outcome = FetchOutcome::Ok;
  size_t length = 0;
  if (!transport_.write(request_.data(), request_.size())) {
    outcome = FetchOutcome::RequestFailed;
  } else {
    LimitedSource head_input(transport_.input(), kMaxHeadBytes);
    ResponseHead head;
    if (snapshot) {
      status_.last_http_status = 0;
    }
    const HeadResult head_result = read_response_head(head_input, head);
    transfer.http_status = static_cast<int16_t>(head.status);
    transfer.expected_bytes = head.content_length;
    if (head_result != HeadResult::Ok) {
      outcome = FetchOutcome::BadHead;
    } else {
      if (snapshot) {
        status_.last_http_status = head.status;
      }
      if (head.status != 200) {
        outcome = FetchOutcome::HttpError;
      } else if (head.content_length < 0 || static_cast<size_t>(head.content_length) > capacity) {
        outcome = FetchOutcome::TooLarge;
      } else {
        length = transport_.input().readBytes(reinterpret_cast<char*>(buffer), static_cast<size_t>(head.content_length));
        transfer.received_bytes = static_cast<uint32_t>(length);
        if (length != static_cast<size_t>(head.content_length)) {
          outcome = FetchOutcome::Truncated;
        }
      }
    }
  }
  transfer.read_end = transport_.read_end();
  transport_.close();
  if (outcome != FetchOutcome::Ok) {
    return outcome;
  }
  const WireResult result = snapshot ? decode_snapshot(buffer, length, settings_.key, nonce, snapshot_)
                                     : basemap_.load(buffer, length, settings_.key, nonce);
  if (result != WireResult::Ok) {
    transfer.rejection = result;
    if (snapshot) {
      status_.last_rejection = result;
    }
    return FetchOutcome::Rejected;
  }
  return FetchOutcome::Ok;
}

void App::build_view(ViewModel& view) {
  advance_uptime();
  const int64_t now = time_.unix_time();
  view = ViewModel{};

  const bool is_recent = snapshot_recent();
  Situation situation;
  situation.alert = is_recent ? snapshot_.alert_level : AlertLevel::Unknown;
  situation.alert_fresh = is_recent && !snapshot_.alerts_stale && snapshot_.alert_level != AlertLevel::Unknown;
  situation.threats_fresh = is_recent && !snapshot_.threats_stale;
  situation.threats_in_watch = snapshot_.threats_in_watch;
  situation.map_requested = map_requested_;
  const ScreenPlan plan = plan_screen(situation);
  view.panel = plan.panel;
  view.banner = plan.banner;
  view.threats_fresh = situation.threats_fresh;
  view.snapshot = &snapshot_;
  view.basemap = &basemap_;

  const uint32_t since = since_valid_s();
  if (since != kNever) {
    if (snapshot_.threats_age_s != kAgeUnknown) {
      view.threats_age_s = add_saturating(snapshot_.threats_age_s, since);
    }
    if (snapshot_.alerts_age_s != kAgeUnknown) {
      view.alert_age_s = add_saturating(snapshot_.alerts_age_s, since);
    }
    if (snapshot_.alert_duration_s != kDurationUnknown &&
        (view.banner == Banner::Yellow || view.banner == Banner::Red)) {
      view.alert_duration_s = add_saturating(snapshot_.alert_duration_s, since);
    }
  }
  if (now > 0) {
    view.time_valid = time_.to_local(now, view.local);
  }
}

}  // namespace core
