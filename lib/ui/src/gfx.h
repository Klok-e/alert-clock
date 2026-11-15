#pragma once

// Single include point for LovyanGFX. Its headers (notably the PC/SDL backend) trip
// warnings this project builds with as errors, so they are silenced here only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#include <LovyanGFX.hpp>
#pragma GCC diagnostic pop
