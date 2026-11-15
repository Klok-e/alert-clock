#pragma once

// Single include point for ArduinoJson, so its configuration and warning
// suppressions stay consistent. Only the PC scenario program uses it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#include <ArduinoJson.h>
#pragma GCC diagnostic pop
