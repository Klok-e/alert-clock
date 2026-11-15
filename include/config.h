#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
// A message rather than #warning, so -Werror builds still work without secrets.
#pragma message("include/secrets.h is missing: building with placeholders from secrets.example.h")
#include "secrets.example.h"
#endif
