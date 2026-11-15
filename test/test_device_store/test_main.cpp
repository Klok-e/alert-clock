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
  TEST_ASSERT_EQUAL_MEMORY(original.data(),EEPROM.staged.data(),86);
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
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_existing_gateway_survives_output_save_and_corruption_falls_back);
  RUN_TEST(test_failed_commits_restore_cache_and_staging_before_an_unrelated_save);
  return UNITY_END();
}
