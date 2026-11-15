#include "host_platform.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <ctime>

namespace {

constexpr int kTimeoutSeconds = 5;

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

SocketTransport::~SocketTransport() { close(); }

bool SocketTransport::connect(const core::Url& url) {
  close();
  if (url.tls) {
    return false;
  }
  char port[8];
  std::snprintf(port, sizeof(port), "%u", static_cast<unsigned>(url.port));
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(url.host.c_str(), port, &hints, &found) != 0) {
    return false;
  }
  for (addrinfo* a = found; a != nullptr; a = a->ai_next) {
    const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) {
      continue;
    }
    timeval timeout{kTimeoutSeconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
      fd_ = fd;
      break;
    }
    ::close(fd);
  }
  freeaddrinfo(found);
  return fd_ >= 0;
}

bool SocketTransport::write(const char* data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    const ssize_t n = ::send(fd_, data + sent, length - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

int SocketTransport::read() {
  if (position_ < buffered_) {
    return static_cast<unsigned char>(buffer_[position_++]);
  }
  if (fd_ < 0) {
    return -1;
  }
  for (;;) {
    const ssize_t n = ::recv(fd_, buffer_, sizeof(buffer_), 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return -1;  // closed, error or timeout
    }
    buffered_ = n;
    position_ = 1;
    return static_cast<unsigned char>(buffer_[0]);
  }
}

void SocketTransport::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  buffered_ = 0;
  position_ = 0;
}

int http_get_status(const std::string& url_text) {
  core::Url url;
  core::RequestBuffer request;
  if (!core::parse_url(url_text.c_str(), url) || !core::build_get_request(url, {}, request)) {
    return -1;
  }
  SocketTransport transport;
  if (!transport.connect(url) || !transport.write(request.data(), request.size())) {
    return -1;
  }
  core::ResponseHead head;
  if (core::read_response_head(transport.input(), head) != core::HeadResult::Ok) {
    return -1;
  }
  return head.status;
}
