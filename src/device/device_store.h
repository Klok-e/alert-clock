#pragma once

#include <cstddef>
#include "output_policy.h"

namespace gateway_store {
constexpr size_t kMaxAddress = 63;
void begin();
const char* load();
bool save(const char* address);
}
namespace output_store {
const core::OutputSettings& load();
bool save(const core::OutputSettings& settings);
}
