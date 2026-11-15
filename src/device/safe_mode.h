#pragma once

#include <cstdint>

// Counts crash restarts in RTC memory, which survives resets but not power loss.
// After kCrashBootsForSafeMode in a row the clock starts with only Wi-Fi, OTA and the
// web service, so a bad build can always be replaced over the network. A normal
// restart (including the one after an OTA update) or a power cycle clears the count.
class SafeMode {
 public:
  static constexpr uint32_t kCrashBootsForSafeMode = 3;

  void begin();
  bool active() const { return active_; }
  uint32_t crash_boots() const { return state_.crash_boots; }
  // Call right before an intentional abort, so the next boot counts it.
  void record_crash();
  // Call once the firmware has run normally for a while.
  void mark_healthy();

 private:
  struct State {
    uint32_t magic;
    uint32_t crash_boots;
    uint32_t pending_crash;
    uint32_t check;
  };

  void save();
  static uint32_t checksum(const State& state);

  State state_{};
  bool active_ = false;
  bool healthy_ = false;
};
