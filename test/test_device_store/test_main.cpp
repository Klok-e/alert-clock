#include <unity.h>
#include "../../src/device/device_store.cpp"
void setUp() { EEPROM.reset(); }
void tearDown() {}
void test_existing_gateway_survives_output_save_and_corruption_falls_back() {
  EEPROM.persisted[0]=0x31; EEPROM.persisted[1]=0x59; EEPROM.persisted[2]=0x57; EEPROM.persisted[3]=0x47;
  const char address[]="old-gateway:8080";
  for (size_t i=0; i<sizeof(address); ++i) EEPROM.persisted[4+i]=address[i];
  const auto original=EEPROM.persisted;
  gateway_store::begin();
  TEST_ASSERT_EQUAL_STRING(address,gateway_store::load());
  TEST_ASSERT_EQUAL_UINT8(5,output_store::load().night.screen_percent);
  TEST_ASSERT_EQUAL_UINT(0,EEPROM.commits);
  core::OutputSettings settings; settings.night_start=1200; settings.day.screen_percent=33;
  TEST_ASSERT_TRUE(output_store::save(settings));
  TEST_ASSERT_EQUAL_MEMORY(original.data(),EEPROM.persisted.data(),68);
  gateway_store::begin();
  TEST_ASSERT_EQUAL_UINT16(1200,output_store::load().night_start);
  TEST_ASSERT_EQUAL_UINT8(33,output_store::load().day.screen_percent);
  EEPROM.persisted[73]^=1;
  gateway_store::begin();
  TEST_ASSERT_EQUAL_UINT8(80,output_store::load().day.screen_percent);
  TEST_ASSERT_EQUAL_STRING(address,gateway_store::load());
}
void test_failed_commits_restore_cache_and_staging_before_an_unrelated_save() {
  gateway_store::begin();
  TEST_ASSERT_TRUE(gateway_store::save("original"));
  core::OutputSettings old; old.day.screen_percent=31;
  TEST_ASSERT_TRUE(output_store::save(old));
  const auto original=EEPROM.persisted;
  EEPROM.commit_ok=false;
  core::OutputSettings rejected=old; rejected.day.screen_percent=89;
  TEST_ASSERT_FALSE(output_store::save(rejected));
  TEST_ASSERT_EQUAL_UINT8(31,output_store::load().day.screen_percent);
  TEST_ASSERT_EQUAL_MEMORY(original.data(),EEPROM.staged.data(),89);
  EEPROM.commit_ok=true;
  TEST_ASSERT_TRUE(gateway_store::save("accepted"));
  gateway_store::begin();
  TEST_ASSERT_EQUAL_UINT8(31,output_store::load().day.screen_percent);
  EEPROM.commit_ok=false;
  TEST_ASSERT_FALSE(gateway_store::save("rejected"));
  TEST_ASSERT_EQUAL_STRING("accepted",gateway_store::load());
  EEPROM.commit_ok=true;
  TEST_ASSERT_TRUE(output_store::save(old));
  gateway_store::begin();
  TEST_ASSERT_EQUAL_STRING("accepted",gateway_store::load());
  old.night_start=old.night_end;
  TEST_ASSERT_FALSE(output_store::save(old));
}
void test_version_1_record_loads_full_volume_and_volume_settings_persist() {
  // A record saved by firmware before volume existed: day screen 33%, night 23:00-10:00.
  const uint8_t v1[18]={'O','U','T','1',1, 33,100,7, 5,5,0, 0x64,0x05, 0x58,0x02, 0, 0,0};
  for (size_t i=0; i<16; ++i) EEPROM.persisted[68+i]=v1[i];
  const uint16_t crc=crc16(v1,16);
  EEPROM.persisted[84]=static_cast<uint8_t>(crc); EEPROM.persisted[85]=static_cast<uint8_t>(crc>>8);
  gateway_store::begin();
  TEST_ASSERT_EQUAL_UINT8(33,output_store::load().day.screen_percent);
  TEST_ASSERT_EQUAL_UINT8(100,output_store::load().day.volume_percent);
  TEST_ASSERT_EQUAL_UINT8(100,output_store::load().night.volume_percent);
  TEST_ASSERT_TRUE(output_store::load().sound_on);
  core::OutputSettings settings; settings.day.volume_percent=25; settings.night.volume_percent=60;
  settings.sound_on=false;
  TEST_ASSERT_TRUE(output_store::save(settings));
  gateway_store::begin();
  TEST_ASSERT_EQUAL_UINT8(25,output_store::load().day.volume_percent);
  TEST_ASSERT_EQUAL_UINT8(60,output_store::load().night.volume_percent);
  TEST_ASSERT_FALSE(output_store::load().sound_on);
  settings.day.volume_percent=0;
  TEST_ASSERT_FALSE(output_store::save(settings));
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_existing_gateway_survives_output_save_and_corruption_falls_back);
  RUN_TEST(test_failed_commits_restore_cache_and_staging_before_an_unrelated_save);
  RUN_TEST(test_version_1_record_loads_full_volume_and_volume_settings_persist);
  return UNITY_END();
}
