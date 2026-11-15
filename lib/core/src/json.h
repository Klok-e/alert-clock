#pragma once

// Single include point for ArduinoJson, so its configuration and warning
// suppressions stay consistent. The ARDUINOJSON_* macros are set globally in
// platformio.ini because they must match in every translation unit.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#include <ArduinoJson.h>
#pragma GCC diagnostic pop
