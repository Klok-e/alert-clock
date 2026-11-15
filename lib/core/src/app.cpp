#include "app.h"

#include <cstdio>
#include <cstring>

#include <etl/string.h>

namespace core {
namespace {

constexpr size_t kTickBytes = 256;
constexpr uint32_t kConnectMs = 2000;
constexpr uint32_t kIdleMs = 5000;
constexpr uint32_t kTotalMs = 10000;
constexpr const char* kDefaultPort = "8080";

// Saturates below kNever, which means "no data yet".
uint32_t age_s(uint64_t now_ms, uint64_t then_ms) {
  const uint64_t seconds = (now_ms - then_ms) / 1000;
  return seconds < kNever ? static_cast<uint32_t>(seconds) : kNever - 1;
}

uint32_t add_saturating(uint32_t a, uint32_t b) { return a > kNever - 1 - b ? kNever - 1 : a + b; }

bool gateway_urls(const char* address, Url& snapshot, Url& basemap) {
  if (address == nullptr || *address == '\0' || std::strlen(address) > 70 ||
      std::strchr(address, '/') != nullptr) {
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
  if (text.full() || basemap_text.full() || !parse_url(text.c_str(), snapshot) ||
      !parse_url(basemap_text.c_str(), basemap)) {
    return false;
  }
  return true;
}

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

bool valid_gateway_address(const char* address) {
  Url snapshot;
  Url basemap;
  return gateway_urls(address, snapshot, basemap);
}

bool App::set_gateway(const char* address) {
  Url snapshot;
  Url basemap;
  if (!gateway_urls(address, snapshot, basemap)) {
    return false;
  }
  if (fetching()) {
    transport_.close();
    phase_ = Phase::Idle;
    ++completed_requests_;
  }
  basemap_wanted_ = false;
  basemap_failed_ = false;
  requested_ = false;
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
  if (fetching()) {
    progress(now);
    return;
  }
  if (basemap_wanted_ && !forced_) {
    basemap_wanted_ = false;
    start(Request::Basemap, now);
    return;
  }
  const uint32_t interval = last_poll_failed_ ? settings_.retry_ms : settings_.poll_ms;
  if (forced_ || !requested_ || now - last_request_ms_ >= interval) {
    forced_ = false;
    requested_ = true;
    last_request_ms_ = now;
    start(Request::Snapshot, now);
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
  basemap_wanted_ = false;
  if (snapshot_.basemap_version == 0) {
    basemap_.invalidate();
  } else if (basemap_.version() != snapshot_.basemap_version &&
             (!basemap_failed_ || time_.millis() - basemap_failed_at_ >= settings_.basemap_retry_ms)) {
    basemap_wanted_ = true;
  }
}

void App::start(Request request, uint32_t now) {
  active_request_ = request;
  transfer_ = TransferStatus{};
  started_ms_ = progress_ms_ = now;
  written_ = 0;
  phase_ = Phase::Connect;
  const bool snapshot = request == Request::Snapshot;
  if (!snapshot) {
    basemap_.invalidate();
  } else {
    status_.last_http_status = 0;
    status_.last_rejection = WireResult::Ok;
  }
  nonce_ = random_.next();
  char query[16];
  std::snprintf(query, sizeof(query), "n=%08lx", static_cast<unsigned long>(nonce_));
  RequestOptions options;
  options.query = query;
  const Url& url = snapshot ? snapshot_url_ : basemap_url_;
  if (!build_get_request(url, options, request_)) {
    finish(FetchOutcome::RequestFailed, now);
  } else if (!transport_.begin(url, now)) {
    finish(FetchOutcome::ConnectFailed, now);
  }
}

void App::finish(FetchOutcome outcome, uint32_t now) {
  const bool snapshot = active_request_ == Request::Snapshot;
  if (transfer_.read_end == ReadEnd::None) {
    transfer_.read_end = transport_.read_end();
  }
  transport_.close();
  if (outcome == FetchOutcome::Ok) {
    const WireResult result = snapshot
        ? decode_snapshot(reply_, transfer_.received_bytes, settings_.key, nonce_, snapshot_)
        : basemap_.load(basemap_body_, transfer_.received_bytes, settings_.key, nonce_);
    if (result != WireResult::Ok) {
      transfer_.rejection = result;
      outcome = FetchOutcome::Rejected;
    }
  }
  transfer_.outcome = outcome;
  transfer_.duration_ms = now - started_ms_;
  phase_ = Phase::Idle;
  ++completed_requests_;
  if (snapshot) {
    status_.last_outcome = outcome;
    status_.last_http_status = transfer_.http_status;
    status_.last_rejection = transfer_.rejection;
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
  } else {
    basemap_failed_ = outcome != FetchOutcome::Ok;
    basemap_failed_at_ = now;
    basemap_status_.last = transfer_;
    if (basemap_failed_) {
      ++basemap_status_.failures;
      basemap_status_.last_failure = transfer_;
    } else {
      ++basemap_status_.successes;
    }
  }
}

void App::progress(uint32_t now) {
  if (now - started_ms_ >= kTotalMs ||
      (phase_ == Phase::Connect && now - started_ms_ >= kConnectMs)) {
    transfer_.read_end = ReadEnd::Timeout;
    finish(phase_ == Phase::Connect ? FetchOutcome::ConnectFailed
           : phase_ == Phase::Write ? FetchOutcome::RequestFailed
           : phase_ == Phase::Head ? FetchOutcome::BadHead : FetchOutcome::Truncated, now);
    return;
  }
  if (phase_ == Phase::Connect) {
    const LinkState state = transport_.advance(now);
    if (state == LinkState::Failed) {
      finish(FetchOutcome::ConnectFailed, now);
    } else if (state == LinkState::Ready) {
      phase_ = Phase::Write;
      progress_ms_ = now;
    }
    return;
  }
  if (phase_ == Phase::Write) {
    const size_t remaining = request_.size() - written_;
    const size_t offered = remaining < kTickBytes ? remaining : kTickBytes;
    const IoResult result = transport_.write_some(
        reinterpret_cast<const uint8_t*>(request_.data()) + written_, offered);
    if (result.state == IoState::Pending) {
      if (now - progress_ms_ >= kIdleMs) {
        transfer_.read_end = ReadEnd::Timeout;
        finish(FetchOutcome::RequestFailed, now);
      }
      return;
    }
    if (result.state != IoState::Progress || result.count == 0 || result.count > offered) {
      finish(FetchOutcome::RequestFailed, now);
      return;
    }
    written_ = static_cast<uint16_t>(written_ + result.count);
    progress_ms_ = now;
    if (written_ == request_.size()) {
      head_parser_.reset(request_);
      phase_ = Phase::Head;
    }
    return;
  }
  size_t budget = kTickBytes;
  while (budget != 0 && fetching()) {
    const bool header = phase_ == Phase::Head;
    uint8_t byte = 0;
    uint8_t* body = active_request_ == Request::Snapshot ? reply_ : basemap_body_;
    const size_t offered = header ? 1 :
        (static_cast<size_t>(transfer_.expected_bytes) - transfer_.received_bytes < budget
          ? static_cast<size_t>(transfer_.expected_bytes) - transfer_.received_bytes : budget);
    const IoResult result = transport_.read_some(header ? &byte : body + transfer_.received_bytes, offered);
    if (result.state == IoState::Pending) {
      if (now - progress_ms_ >= kIdleMs) {
        transfer_.read_end = ReadEnd::Timeout;
        finish(header ? FetchOutcome::BadHead : FetchOutcome::Truncated, now);
      }
      return;
    }
    if (result.state != IoState::Progress || result.count == 0 || result.count > offered) {
      finish(header ? FetchOutcome::BadHead : FetchOutcome::Truncated, now);
      return;
    }
    budget -= result.count;
    progress_ms_ = now;
    if (header) {
      const HeadResult parsed = head_parser_.feed(request_, byte);
      if (parsed == HeadResult::Malformed) {
        finish(FetchOutcome::BadHead, now);
      } else if (parsed == HeadResult::Ok) {
        const ResponseHead& head = head_parser_.head();
        transfer_.http_status = static_cast<int16_t>(head.status);
        transfer_.expected_bytes = head.content_length;
        const size_t capacity = active_request_ == Request::Snapshot ? sizeof(reply_) : sizeof(basemap_body_);
        if (head.status != 200) {
          finish(FetchOutcome::HttpError, now);
        } else if (head.content_length < 0 || static_cast<size_t>(head.content_length) > capacity) {
          finish(FetchOutcome::TooLarge, now);
        } else if (head.content_length == 0) {
          finish(FetchOutcome::Ok, now);
        } else {
          phase_ = Phase::Body;
        }
      }
    } else {
      transfer_.received_bytes += static_cast<uint32_t>(result.count);
      if (transfer_.received_bytes == static_cast<uint32_t>(transfer_.expected_bytes)) {
        finish(FetchOutcome::Ok, now);
      }
    }
  }
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
