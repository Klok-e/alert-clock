#include "safe_mode.h"

#include <Esp.h>
#include <user_interface.h>

namespace {

constexpr uint32_t kMagic = 0x53414645;  // "SAFE"
// RTC user memory words 0..31 hold the OTA bootloader command (eboot), so stay above them.
constexpr uint32_t kRtcOffsetWords = 64;

bool is_crash(uint32_t reason) {
  return reason == REASON_WDT_RST || reason == REASON_EXCEPTION_RST || reason == REASON_SOFT_WDT_RST;
}

}  // namespace

uint32_t SafeMode::checksum(const State& state) {
  return (state.magic ^ (state.crash_boots * 2654435761u) ^ (state.pending_crash << 7)) + 0x9E3779B9u;
}

void SafeMode::save() {
  state_.check = checksum(state_);
  ESP.rtcUserMemoryWrite(kRtcOffsetWords, reinterpret_cast<uint32_t*>(&state_), sizeof(state_));
}

void SafeMode::begin() {
  ESP.rtcUserMemoryRead(kRtcOffsetWords, reinterpret_cast<uint32_t*>(&state_), sizeof(state_));
  if (state_.magic != kMagic || state_.check != checksum(state_)) {
    state_ = State{kMagic, 0, 0, 0};
  }
  const rst_info* info = ESP.getResetInfoPtr();
  const bool crashed = state_.pending_crash != 0 || (info != nullptr && is_crash(info->reason));
  state_.pending_crash = 0;
  state_.crash_boots = crashed ? state_.crash_boots + 1 : 0;
  active_ = state_.crash_boots >= kCrashBootsForSafeMode;
  save();
}

void SafeMode::record_crash() {
  state_.pending_crash = 1;
  save();
}

void SafeMode::mark_healthy() {
  if (healthy_) {
    return;
  }
  healthy_ = true;
  state_.crash_boots = 0;
  save();
}
