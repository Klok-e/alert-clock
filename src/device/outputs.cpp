#include "outputs.h"
#include <Arduino.h>
#include <core_esp8266_waveform.h>
#include <esp8266_peri.h>

namespace {

// The WS2812 is bit-banged with maskable interrupts off, but the Wi-Fi NMI still lands in
// 2-3% of frames and stalls the CPU for about 2 us, so the LED reads a 0 bit as 1 and shows a
// wrong colour. This clock's LED was tested to ignore a frame shorter than 24 bits, so a frame
// stops at the first late bit and is sent again; a late last bit has already latched and is sent
// again too.
constexpr uint32_t kBitCycles = F_CPU / 800000;   // 1.25 us
constexpr uint32_t kZeroHigh = F_CPU / 2500000;   // 0.4 us
constexpr uint32_t kOneHigh = F_CPU / 1250000;    // 0.8 us
constexpr uint32_t kHighSlack = kBitCycles / 8;   // 0.16 us: a 0 bit stays under the 0.55 us limit
constexpr uint32_t kLowSlack = F_CPU / 200000;    // 5 us; a longer low may latch a partial frame
constexpr uint32_t kLatchUs = 300;
constexpr uint8_t kAttempts = 4;                  // measured: one resend always sufficed
static_assert(board::kLedPin < 16, "GPOS and GPOC cover GPIO0-15");

// Returns false when a bit ran late; the frame stops at that bit. noinline keeps it in IRAM:
// inlined into a flash caller, a cache miss stretched the first bit.
__attribute__((noinline)) IRAM_ATTR bool send_frame(uint32_t grb) {
  const uint32_t pin = 1U << board::kLedPin;
  bool on_time = true;
  const uint32_t saved = xt_rsil(15);
  uint32_t start = esp_get_cycle_count() - kBitCycles;
  for (uint32_t mask = 1U << 23; mask != 0; mask >>= 1) {
    const uint32_t high = (grb & mask) != 0 ? kOneHigh : kZeroHigh;
    uint32_t now;
    while ((now = esp_get_cycle_count()) - start < kBitCycles) {}
    GPOS = pin;
    // A stall before the rising edge would shorten the high phase and turn a 1 into a 0.
    if (esp_get_cycle_count() - now > kHighSlack || now - start > kBitCycles + kLowSlack) on_time = false;
    start = now;
    while ((now = esp_get_cycle_count()) - start < high) {}
    GPOC = pin;
    if (esp_get_cycle_count() - start > high + kHighSlack) on_time = false;
    if (!on_time) break;
  }
  while (esp_get_cycle_count() - start < kBitCycles) {}
  xt_wsr_ps(saved);
  return on_time;
}

}  // namespace

void Outputs::begin() {
  pinMode(board::kBuzzerPin, OUTPUT);
  digitalWrite(board::kBuzzerPin, LOW);
  pinMode(board::kButtonPin, INPUT);
  pinMode(board::kBacklightPin, OUTPUT);
  analogWriteRange(1023);
  backlight(0);
}
void Outputs::begin_led() {
  pinMode(board::kLedPin, OUTPUT);
  digitalWrite(board::kLedPin, LOW);
  led_sent_us_ = micros();
  led(0, 0, 0);
}
void Outputs::backlight(uint8_t percent) {
  if (percent > 100) percent = 100;
  analogWrite(board::kBacklightPin, 1023 - (1023 * percent) / 100);
}
void Outputs::buzzer(uint16_t frequency_hz, uint8_t volume_percent) {
  if (frequency_hz != 0) {
    // tone() always drives a 50% duty cycle; a shorter high time plays the piezo more quietly.
    const uint32_t period = microsecondsToClockCycles(1000000UL) / frequency_hz;
    uint32_t high = period * core::buzzer_duty_permille(volume_percent) / 1000U;
    const uint32_t shortest = microsecondsToClockCycles(2);
    if (high < shortest) high = shortest;
    startWaveformClockCycles(board::kBuzzerPin, high, period - high);
  } else {
    noTone(board::kBuzzerPin);
    digitalWrite(board::kBuzzerPin, LOW);
  }
}
void Outputs::led(uint8_t red, uint8_t green, uint8_t blue) {
  const uint32_t grb = static_cast<uint32_t>(green) << 16 | static_cast<uint32_t>(red) << 8 | blue;
  for (uint8_t attempt = 0; attempt < kAttempts; ++attempt) {
    while (micros() - led_sent_us_ < kLatchUs) {}
    const bool on_time = send_frame(grb);
    led_sent_us_ = micros();
    if (on_time) return;
  }
}
