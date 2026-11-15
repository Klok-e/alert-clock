#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

constexpr size_t kSha256Bytes = 32;

// FIPS 180-4 SHA-256 on fixed storage.
class Sha256 {
 public:
  Sha256();
  void update(const uint8_t* data, size_t length);
  void finish(uint8_t (&out)[kSha256Bytes]);

 private:
  void block(const uint8_t* data);

  uint32_t state_[8];
  uint8_t buffer_[64];
  uint64_t total_ = 0;
  size_t buffered_ = 0;
};

// RFC 2104 HMAC-SHA256 with a 32-byte key.
void hmac_sha256(const uint8_t (&key)[kSha256Bytes], const uint8_t* data, size_t length,
                 uint8_t (&out)[kSha256Bytes]);

// Compares in time independent of where the first difference is.
bool equal_digest(const uint8_t* a, const uint8_t* b, size_t length);

}  // namespace core
