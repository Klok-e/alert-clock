#include "device_store.h"

#include <EEPROM.h>
#include <cstring>

namespace {
constexpr size_t kGatewayBytes = 68;
constexpr size_t kOutputBytes = 18;
constexpr size_t kStoreBytes = kGatewayBytes + kOutputBytes;
constexpr uint32_t kGatewayMagic = 0x47575931;
char address_cache[gateway_store::kMaxAddress + 1] = {};
core::OutputSettings output_cache;
bool ready = false;

uint16_t crc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
  }
  return crc;
}
void encode(const core::OutputSettings& settings, uint8_t (&bytes)[kOutputBytes]) {
  bytes[0] = 'O'; bytes[1] = 'U'; bytes[2] = 'T'; bytes[3] = '1'; bytes[4] = 1;
  bytes[5] = settings.day.screen_percent;
  bytes[6] = settings.day.led_percent;
  bytes[7] = settings.day.sound_mask;
  bytes[8] = settings.night.screen_percent;
  bytes[9] = settings.night.led_percent;
  bytes[10] = settings.night.sound_mask;
  bytes[11] = static_cast<uint8_t>(settings.night_start);
  bytes[12] = static_cast<uint8_t>(settings.night_start >> 8);
  bytes[13] = static_cast<uint8_t>(settings.night_end);
  bytes[14] = static_cast<uint8_t>(settings.night_end >> 8);
  bytes[15] = 0;
  const uint16_t crc = crc16(bytes, 16);
  bytes[16] = static_cast<uint8_t>(crc);
  bytes[17] = static_cast<uint8_t>(crc >> 8);
}
bool decode(const uint8_t (&bytes)[kOutputBytes], core::OutputSettings& settings) {
  if (bytes[0] != 'O' || bytes[1] != 'U' || bytes[2] != 'T' || bytes[3] != '1' ||
      bytes[4] != 1 || bytes[15] != 0 ||
      crc16(bytes, 16) != static_cast<uint16_t>(bytes[16] | bytes[17] << 8)) return false;
  core::OutputSettings candidate;
  candidate.day = {bytes[5], bytes[6], bytes[7]};
  candidate.night = {bytes[8], bytes[9], bytes[10]};
  candidate.night_start = static_cast<uint16_t>(bytes[11] | bytes[12] << 8);
  candidate.night_end = static_cast<uint16_t>(bytes[13] | bytes[14] << 8);
  if (!core::valid_output_settings(candidate)) return false;
  settings = candidate;
  return true;
}
bool commit_bytes(size_t offset, const uint8_t* bytes, size_t length) {
  if (!ready || length > kGatewayBytes) return false;
  uint8_t previous[kGatewayBytes];
  for (size_t i = 0; i < length; ++i) {
    previous[i] = EEPROM.read(static_cast<int>(offset + i));
    EEPROM.write(static_cast<int>(offset + i), bytes[i]);
  }
  if (EEPROM.commit()) return true;
  // A later unrelated commit must not persist this rejected candidate.
  for (size_t i = 0; i < length; ++i)
    EEPROM.write(static_cast<int>(offset + i), previous[i]);
  return false;
}
}
namespace gateway_store {
void begin() {
  address_cache[0] = '\0';
  output_cache = core::OutputSettings{};
  EEPROM.begin(kStoreBytes);
  ready = EEPROM.getConstDataPtr() != nullptr && EEPROM.length() >= kStoreBytes;
  if (!ready) return;
  uint32_t magic = 0;
  EEPROM.get(0, magic);
  if (magic == kGatewayMagic) {
    for (size_t i = 0; i <= kMaxAddress; ++i)
      address_cache[i] = static_cast<char>(EEPROM.read(static_cast<int>(4 + i)));
    address_cache[kMaxAddress] = '\0';
  }
  uint8_t bytes[kOutputBytes];
  for (size_t i = 0; i < kOutputBytes; ++i)
    bytes[i] = EEPROM.read(static_cast<int>(kGatewayBytes + i));
  decode(bytes, output_cache);
}
const char* load() { return address_cache; }
bool save(const char* address) {
  if (address == nullptr) return false;
  const size_t length = strnlen(address, kMaxAddress + 1);
  if (length > kMaxAddress) return false;
  uint8_t bytes[kGatewayBytes] = {};
  for (size_t i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(kGatewayMagic >> (i * 8));
  for (size_t i = 0; i < length; ++i) bytes[4 + i] = static_cast<uint8_t>(address[i]);
  if (!commit_bytes(0, bytes, sizeof(bytes))) return false;
  std::memcpy(address_cache, bytes + 4, sizeof(address_cache));
  return true;
}
}
namespace output_store {
const core::OutputSettings& load() { return output_cache; }
bool save(const core::OutputSettings& settings) {
  if (!core::valid_output_settings(settings)) return false;
  uint8_t bytes[kOutputBytes];
  encode(settings, bytes);
  if (!commit_bytes(kGatewayBytes, bytes, sizeof(bytes))) return false;
  output_cache = settings;
  return true;
}
}
