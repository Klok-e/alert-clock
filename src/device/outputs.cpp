#include "outputs.h"
#include <Arduino.h>

void Outputs::begin() {
  pinMode(board::kBuzzerPin, OUTPUT);
  digitalWrite(board::kBuzzerPin, LOW);
  pinMode(board::kButtonPin, INPUT);
  pinMode(board::kBacklightPin, OUTPUT);
  analogWriteRange(1023);
  backlight(0);
}
void Outputs::begin_led() {
  led_.begin();
  led_.clear();
  led_.show();
}
void Outputs::backlight(uint8_t percent) {
  if (percent > 100) percent = 100;
  analogWrite(board::kBacklightPin, 1023 - (1023 * percent) / 100);
}
void Outputs::buzzer(uint16_t frequency_hz) {
  if (frequency_hz != 0) tone(board::kBuzzerPin, frequency_hz);
  else {
    noTone(board::kBuzzerPin);
    digitalWrite(board::kBuzzerPin, LOW);
  }
}
void Outputs::led(uint8_t red, uint8_t green, uint8_t blue) {
  led_.setPixelColor(0, Adafruit_NeoPixel::Color(red, green, blue));
  led_.show();
}
