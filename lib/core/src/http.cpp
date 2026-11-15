#include "http.h"

#include <cstring>

namespace core {
namespace {

constexpr size_t kMaxLine = 255;
constexpr int kMaxHeaderLines = 64;
using Line = etl::string<kMaxLine>;

bool starts_with(const char* text, const char* prefix) {
  return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

bool is_host_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.';
}

// Printable ASCII except space: safe in a request line or header value.
bool is_visible(char c) { return c > 0x20 && c < 0x7f; }

bool append(RequestBuffer& out, const char* text) {
  const size_t length = std::strlen(text);
  if (length > out.available()) {
    return false;
  }
  out.append(text, length);
  return true;
}

char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool name_equals(const Line& line, size_t name_length, const char* expected) {
  if (std::strlen(expected) != name_length) {
    return false;
  }
  for (size_t i = 0; i < name_length; ++i) {
    if (to_lower(line[i]) != expected[i]) {
      return false;
    }
  }
  return true;
}

enum class LineResult : uint8_t { Ok, Closed, Truncated };

// Reads up to LF and strips a trailing CR. A line longer than the buffer is read to its
// end and reported as Truncated.
LineResult read_line(ByteSource& in, Line& line) {
  line.clear();
  bool truncated = false;
  for (;;) {
    const int c = in.read();
    if (c < 0) {
      return LineResult::Closed;
    }
    if (c == '\n') {
      break;
    }
    if (line.full()) {
      truncated = true;
    } else {
      line.push_back(static_cast<char>(c));
    }
  }
  if (truncated) {
    return LineResult::Truncated;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return LineResult::Ok;
}

bool parse_content_length(const Line& line, size_t begin, int32_t& out) {
  if (begin >= line.size() || line.size() - begin > 9) {
    return false;
  }
  int32_t value = 0;
  for (size_t i = begin; i < line.size(); ++i) {
    const char c = line[i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  out = value;
  return true;
}

}  // namespace

bool parse_url(const char* text, Url& out) {
  out = Url{};
  if (text == nullptr) {
    return false;
  }
  const char* p = text;
  if (starts_with(p, "https://")) {
    out.tls = true;
    out.port = 443;
    p += 8;
  } else if (starts_with(p, "http://")) {
    out.port = 80;
    p += 7;
  } else {
    return false;
  }
  while (*p != '\0' && *p != ':' && *p != '/') {
    if (!is_host_char(*p) || out.host.full()) {
      return false;
    }
    out.host.push_back(*p++);
  }
  if (out.host.empty()) {
    return false;
  }
  if (*p == ':') {
    ++p;
    uint32_t port = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      port = port * 10 + static_cast<uint32_t>(*p++ - '0');
      if (++digits > 5) {
        return false;
      }
    }
    if (digits == 0 || port == 0 || port > 65535) {
      return false;
    }
    out.port = static_cast<uint16_t>(port);
  }
  if (*p == '\0') {
    out.path.assign("/");
    return true;
  }
  if (*p != '/') {
    return false;
  }
  for (; *p != '\0'; ++p) {
    if (!is_visible(*p) || out.path.full()) {
      return false;
    }
    out.path.push_back(*p);
  }
  return true;
}

bool build_get_request(const Url& url, const RequestOptions& options, RequestBuffer& out) {
  out.clear();
  if (!append(out, "GET ") || !append(out, url.path.c_str())) {
    return false;
  }
  if (options.query != nullptr && *options.query != '\0') {
    for (const char* p = options.query; *p != '\0'; ++p) {
      if (!is_visible(*p)) {
        return false;
      }
    }
    if (!append(out, "?") || !append(out, options.query)) {
      return false;
    }
  }
  if (!append(out, " HTTP/1.0\r\nHost: ") || !append(out, url.host.c_str())) {
    return false;
  }
  const uint16_t default_port = url.tls ? 443 : 80;
  if (url.port != default_port) {
    char port[8];
    char* end = port + sizeof(port);
    char* p = end;
    *--p = '\0';
    uint16_t value = url.port;
    do {
      *--p = static_cast<char>('0' + value % 10);
      value = static_cast<uint16_t>(value / 10);
    } while (value != 0);
    if (!append(out, ":") || !append(out, p)) {
      return false;
    }
  }
  return append(out, "\r\nUser-Agent: alert-clock/0.2\r\nConnection: close\r\n\r\n");
}

HeadResult read_response_head(ByteSource& in, ResponseHead& head) {
  head = ResponseHead{};
  Line line;
  const LineResult status_line = read_line(in, line);
  if (status_line == LineResult::Closed) {
    return HeadResult::Closed;
  }
  // "HTTP/1.x SSS reason"
  if (status_line == LineResult::Truncated || line.size() < 12 || line.compare(0, 7, "HTTP/1.") != 0 ||
      line[8] != ' ' || (line.size() > 12 && line[12] != ' ')) {
    return HeadResult::Malformed;
  }
  int status = 0;
  for (size_t i = 9; i < 12; ++i) {
    if (line[i] < '0' || line[i] > '9') {
      return HeadResult::Malformed;
    }
    status = status * 10 + (line[i] - '0');
  }
  head.status = status;

  for (int count = 0; count < kMaxHeaderLines; ++count) {
    const LineResult result = read_line(in, line);
    if (result == LineResult::Closed) {
      return HeadResult::Closed;
    }
    if (result == LineResult::Truncated) {
      continue;
    }
    if (line.empty()) {
      return HeadResult::Ok;
    }
    size_t colon = 0;
    while (colon < line.size() && line[colon] != ':') {
      ++colon;
    }
    if (colon == line.size()) {
      return HeadResult::Malformed;
    }
    size_t value = colon + 1;
    while (value < line.size() && (line[value] == ' ' || line[value] == '\t')) {
      ++value;
    }
    size_t end = line.size();
    while (end > value && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
      --end;
    }
    line.resize(end);
    if (name_equals(line, colon, "content-length")) {
      if (!parse_content_length(line, value, head.content_length)) {
        return HeadResult::Malformed;
      }
    }
  }
  return HeadResult::Malformed;
}

}  // namespace core
