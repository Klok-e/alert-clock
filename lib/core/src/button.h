#pragma once

#include <cstdint>

namespace core {

enum class ButtonEvent : uint8_t { None, Short, Long };

constexpr uint32_t kDebounceMs = 30;
constexpr uint32_t kLongPressMs = 800;

// Debounces a raw button level and reports presses. Long fires once while still
// held; releasing after a long press reports nothing more.
class ButtonDetector {
 public:
  ButtonEvent update(bool pressed, uint32_t now_ms);

 private:
  bool stable_ = false;
  bool candidate_ = false;
  uint32_t candidate_since_ = 0;
  uint32_t pressed_at_ = 0;
  bool long_reported_ = false;
};

}  // namespace core
