#pragma once

#include <cstddef>
#include <cstdint>

#include <etl/string.h>

#include "byte_source.h"

namespace core {

struct Url {
  bool tls = false;
  etl::string<64> host;
  uint16_t port = 0;
  etl::string<160> path;
};

// Accepts "http://host[:port]/path" and "https://host[:port]/path".
// Returns false for anything malformed or longer than the fixed buffers.
bool parse_url(const char* text, Url& out);

struct RequestOptions {
  const char* query = nullptr;  // appended after "?", e.g. "n=0a1b2c3d"
};

using RequestBuffer = etl::string<256>;

// Builds an HTTP/1.0 GET with "Connection: close", so servers never reply chunked.
// Returns false if it does not fit or the query holds anything but printable ASCII.
bool build_get_request(const Url& url, const RequestOptions& options, RequestBuffer& out);

struct ResponseHead {
  int status = 0;
  int32_t content_length = -1;  // -1 when absent
};

enum class HeadResult : uint8_t { Ok, Closed, Malformed };

// Reads the status line and headers, stopping after the blank line. Header lines
// longer than the line buffer are skipped, not fatal (servers send long cookies).
HeadResult read_response_head(ByteSource& in, ResponseHead& head);

}  // namespace core
