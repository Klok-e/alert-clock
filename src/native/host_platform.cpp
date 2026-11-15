#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "host_platform.h"

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

namespace {

// glibc may refuse cancellation while its resolver is executing. Keep the one
// request's storage alive until gai_error reports completion, even after close.
struct Resolver {
  gaicb request{};
  addrinfo hints{};
  char host[65]{};
  uint64_t generation = 0;
  uint64_t owner = 0;
  bool active = false;

  void reap() {
    if (active && owner == 0 && gai_error(&request) != EAI_INPROGRESS) {
      if (request.ar_result != nullptr) {
        freeaddrinfo(request.ar_result);
      }
      request.ar_result = nullptr;
      active = false;
    }
  }

  int start(const char* name, uint64_t& token) {
    reap();
    if (active) {
      return 0;
    }
    request = gaicb{};
    hints = addrinfo{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::snprintf(host, sizeof(host), "%s", name);
    request.ar_name = host;
    request.ar_request = &hints;
    gaicb* pending[] = {&request};
    sigevent notification{};
    notification.sigev_notify = SIGEV_NONE;
    if (getaddrinfo_a(GAI_NOWAIT, pending, 1, &notification) != 0) {
      return -1;
    }
    owner = ++generation;
    active = true;
    token = owner;
    return 1;
  }

  core::LinkState take(uint64_t token, addrinfo*& result) {
    if (!active || owner != token) {
      return core::LinkState::Failed;
    }
    const int error = gai_error(&request);
    if (error == EAI_INPROGRESS) {
      return core::LinkState::Pending;
    }
    result = request.ar_result;
    request.ar_result = nullptr;
    active = false;
    owner = 0;
    if (error != 0) {
      if (result != nullptr) {
        freeaddrinfo(result);
        result = nullptr;
      }
      return core::LinkState::Failed;
    }
    return core::LinkState::Ready;
  }

  void retire(uint64_t token) {
    if (active && owner == token) {
      owner = 0;
      (void)gai_cancel(&request);
    }
    reap();
  }
};

Resolver resolver;

bool pending_error() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

uint32_t steady_ms() {
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

bool FakeTime::to_local(int64_t unix_time, core::LocalTime& out) {
  const time_t t = static_cast<time_t>(unix_time);
  struct tm local {};
  if (localtime_r(&t, &local) == nullptr) {
    return false;
  }
  out.year = local.tm_year + 1900;
  out.month = static_cast<uint8_t>(local.tm_mon + 1);
  out.day = static_cast<uint8_t>(local.tm_mday);
  out.hour = static_cast<uint8_t>(local.tm_hour);
  out.minute = static_cast<uint8_t>(local.tm_min);
  out.second = static_cast<uint8_t>(local.tm_sec);
  out.weekday = static_cast<uint8_t>(local.tm_wday);
  return true;
}

uint32_t HostRandom::next() {
  static std::random_device device;
  return device();
}

SocketTransport::~SocketTransport() { close(); }

bool SocketTransport::begin(const core::Url& url, uint32_t) {
  close();
  read_end_ = core::ReadEnd::None;
  if (url.tls) {
    return false;
  }
  host_ = url.host;
  port_ = url.port;
  numeric_ = sockaddr_storage{};
  auto* ipv4 = reinterpret_cast<sockaddr_in*>(&numeric_);
  auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&numeric_);
  if (inet_pton(AF_INET, host_.c_str(), &ipv4->sin_addr) == 1) {
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(port_);
    numeric_length_ = sizeof(sockaddr_in);
  } else if (inet_pton(AF_INET6, host_.c_str(), &ipv6->sin6_addr) == 1) {
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons(port_);
    numeric_length_ = sizeof(sockaddr_in6);
  }
  addresses_ready_ = numeric_length_ != 0;
  state_ = State::Resolve;
  return true;
}

core::LinkState SocketTransport::next_address() {
  const sockaddr* address = nullptr;
  socklen_t length = 0;
  int family = 0;
  if (numeric_length_ != 0) {
    address = reinterpret_cast<const sockaddr*>(&numeric_);
    length = numeric_length_;
    family = address->sa_family;
    numeric_length_ = 0;
  } else if (next_ != nullptr) {
    addrinfo* current = next_;
    next_ = next_->ai_next;
    address = current->ai_addr;
    length = current->ai_addrlen;
    family = current->ai_family;
    if (family == AF_INET) {
      reinterpret_cast<sockaddr_in*>(current->ai_addr)->sin_port = htons(port_);
    } else if (family == AF_INET6) {
      reinterpret_cast<sockaddr_in6*>(current->ai_addr)->sin6_port = htons(port_);
    }
  } else {
    state_ = State::Failed;
    return core::LinkState::Failed;
  }
  fd_ = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    return core::LinkState::Pending;
  }
  if (::connect(fd_, address, length) == 0) {
    state_ = State::Ready;
    return core::LinkState::Ready;
  }
  if (errno == EINPROGRESS || errno == EINTR) {
    state_ = State::Connect;
    return core::LinkState::Pending;
  }
  ::close(fd_);
  fd_ = -1;
  return core::LinkState::Pending;
}

core::LinkState SocketTransport::advance(uint32_t) {
  resolver.reap();
  if (state_ == State::Ready) {
    return core::LinkState::Ready;
  }
  if (state_ == State::Idle || state_ == State::Failed) {
    return core::LinkState::Failed;
  }
  if (state_ == State::Resolve) {
    if (addresses_ready_) {
      return next_address();
    }
    if (resolver_token_ == 0) {
      const int submitted = resolver.start(host_.c_str(), resolver_token_);
      if (submitted < 0) {
        state_ = State::Failed;
        return core::LinkState::Failed;
      }
      return core::LinkState::Pending;
    }
    const core::LinkState resolved = resolver.take(resolver_token_, addresses_);
    if (resolved == core::LinkState::Pending) {
      return resolved;
    }
    resolver_token_ = 0;
    if (resolved == core::LinkState::Failed) {
      state_ = State::Failed;
      return resolved;
    }
    next_ = addresses_;
    addresses_ready_ = true;
    return next_address();
  }
  pollfd socket_event{fd_, POLLOUT, 0};
  const int polled = ::poll(&socket_event, 1, 0);
  if (polled == 0 || (polled < 0 && errno == EINTR)) {
    return core::LinkState::Pending;
  }
  int error = 0;
  socklen_t length = sizeof(error);
  if (polled > 0 && ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) {
    state_ = State::Ready;
    return core::LinkState::Ready;
  }
  ::close(fd_);
  fd_ = -1;
  state_ = State::Resolve;
  return next_address();
}

core::IoResult SocketTransport::write_some(const uint8_t* data, size_t length) {
  if (state_ != State::Ready) {
    return {core::IoState::Failed, 0};
  }
  const ssize_t count = ::send(fd_, data, length, MSG_NOSIGNAL);
  if (count > 0) {
    return {core::IoState::Progress, static_cast<size_t>(count)};
  }
  if (count < 0 && pending_error()) {
    return {core::IoState::Pending, 0};
  }
  read_end_ = core::ReadEnd::Error;
  return {core::IoState::Failed, 0};
}

core::IoResult SocketTransport::read_some(uint8_t* data, size_t capacity) {
  if (state_ != State::Ready) {
    return {core::IoState::Failed, 0};
  }
  const ssize_t count = ::recv(fd_, data, capacity, 0);
  if (count > 0) {
    return {core::IoState::Progress, static_cast<size_t>(count)};
  }
  if (count < 0 && pending_error()) {
    return {core::IoState::Pending, 0};
  }
  read_end_ = count == 0 ? core::ReadEnd::Closed : core::ReadEnd::Error;
  return {count == 0 ? core::IoState::End : core::IoState::Failed, 0};
}

void SocketTransport::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (resolver_token_ != 0) {
    resolver.retire(resolver_token_);
    resolver_token_ = 0;
  } else {
    resolver.reap();
  }
  if (addresses_ != nullptr) {
    freeaddrinfo(addresses_);
  }
  addresses_ = next_ = nullptr;
  addresses_ready_ = false;
  numeric_length_ = 0;
  state_ = State::Idle;
}

int http_get_status(const std::string& url_text, const char* query) {
  core::Url url;
  core::RequestBuffer request;
  core::RequestOptions options;
  options.query = query;
  if (!core::parse_url(url_text.c_str(), url) || !core::build_get_request(url, options, request)) {
    return -1;
  }
  SocketTransport transport;
  const uint32_t started = steady_ms();
  if (!transport.begin(url, started)) {
    return -1;
  }
  enum class Phase { Connect, Write, Head };
  Phase phase = Phase::Connect;
  size_t sent = 0;
  core::HeadParser parser;
  while (steady_ms() - started < 15000) {
    bool progressed = false;
    if (phase == Phase::Connect) {
      const core::LinkState state = transport.advance(steady_ms());
      if (state == core::LinkState::Failed) {
        return -1;
      }
      if (state == core::LinkState::Ready) {
        phase = Phase::Write;
        progressed = true;
      }
    } else if (phase == Phase::Write) {
      const core::IoResult result = transport.write_some(
          reinterpret_cast<const uint8_t*>(request.data()) + sent, request.size() - sent);
      if (result.state == core::IoState::Failed || result.state == core::IoState::End) {
        return -1;
      }
      if (result.state == core::IoState::Progress) {
        sent += result.count;
        progressed = true;
        if (sent == request.size()) {
          parser.reset(request);
          phase = Phase::Head;
        }
      }
    } else {
      uint8_t byte = 0;
      const core::IoResult result = transport.read_some(&byte, 1);
      if (result.state == core::IoState::End || result.state == core::IoState::Failed) {
        return -1;
      }
      if (result.state == core::IoState::Progress) {
        progressed = true;
        const core::HeadResult parsed = parser.feed(request, byte);
        if (parsed == core::HeadResult::Malformed) {
          return -1;
        }
        if (parsed == core::HeadResult::Ok) {
          return parser.head().status;
        }
      }
    }
    if (!progressed) {
      (void)::poll(nullptr, 0, 1);
    }
  }
  return -1;
}
