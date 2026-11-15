#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

// Pull-based byte input shared by the clock (TCP socket), the PC build (TCP socket) and tests.
// read() returns the next byte (0..255), or -1 at end of input or on timeout.
class ByteSource {
 public:
  virtual int read() = 0;

  virtual size_t readBytes(char* buffer, size_t length) {
    size_t count = 0;
    while (count < length) {
      const int c = read();
      if (c < 0) {
        break;
      }
      buffer[count++] = static_cast<char>(c);
    }
    return count;
  }

 protected:
  ByteSource() = default;
  ~ByteSource() = default;
};

// Reads from a fixed in-memory buffer.
class MemorySource final : public ByteSource {
 public:
  MemorySource(const char* data, size_t length) : data_(data), length_(length) {}

  int read() override {
    if (position_ >= length_) {
      return -1;
    }
    return static_cast<unsigned char>(data_[position_++]);
  }

 private:
  const char* data_;
  size_t length_;
  size_t position_ = 0;
};

// Ends the input after `limit` bytes, so a misbehaving server cannot stream forever.
class LimitedSource final : public ByteSource {
 public:
  LimitedSource(ByteSource& inner, size_t limit) : inner_(inner), remaining_(limit) {}

  int read() override {
    if (remaining_ == 0) {
      return -1;
    }
    const int c = inner_.read();
    if (c >= 0) {
      --remaining_;
    }
    return c;
  }

  bool exhausted() const { return remaining_ == 0; }

 private:
  ByteSource& inner_;
  size_t remaining_;
};

}  // namespace core
