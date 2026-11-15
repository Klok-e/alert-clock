#pragma once

// Constant data that must stay out of RAM on the clock: on the ESP8266, PROGMEM data lives in
// flash and is read with the *_P functions; on the PC both are ordinary memory.

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP8266)
#include <pgmspace.h>
#else
#ifndef PROGMEM
#define PROGMEM
#endif
#endif

namespace ui {

// Copies a PROGMEM string into `out` (always terminated).
inline void flash_copy(char* out, size_t size, const char* flash_text) {
#if defined(ARDUINO_ARCH_ESP8266)
  strncpy_P(out, flash_text, size - 1);
#else
  std::strncpy(out, flash_text, size - 1);
#endif
  out[size - 1] = '\0';
}

// snprintf with a PROGMEM format; %s arguments must be in RAM.
inline int flash_format(char* out, size_t size, const char* flash_format_text, ...) {
  va_list args;
  va_start(args, flash_format_text);
#if defined(ARDUINO_ARCH_ESP8266)
  const int length = vsnprintf_P(out, size, flash_format_text, args);
#else
  const int length = std::vsnprintf(out, size, flash_format_text, args);
#endif
  va_end(args);
  return length;
}

}  // namespace ui
