#include "button.h"

namespace core {

ButtonEvent ButtonDetector::update(bool pressed, uint32_t now_ms) {
  if (pressed != candidate_) {
    candidate_ = pressed;
    candidate_since_ = now_ms;
  }
  if (candidate_ != stable_ && now_ms - candidate_since_ >= kDebounceMs) {
    stable_ = candidate_;
    if (stable_) {
      pressed_at_ = now_ms;
      long_reported_ = false;
    } else if (!long_reported_) {
      return ButtonEvent::Short;
    }
  }
  if (stable_ && !long_reported_ && now_ms - pressed_at_ >= kLongPressMs) {
    long_reported_ = true;
    return ButtonEvent::Long;
  }
  return ButtonEvent::None;
}

}  // namespace core
