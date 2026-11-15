#include <unity.h>

#include <vector>

#include "../../src/device/esp_platform.cpp"

void setUp() { fake_esp8266::reset(); }
void tearDown() {}

static void open(EspTransport& transport) {
  core::Url url;
  url.host = "gateway.test";
  url.port = 8080;
  transport.begin();
  TEST_ASSERT_TRUE(transport.connect(url));
}

void test_binary_stream_drains_closing_tail_before_eof() {
  for (const bool deliver_on_delay : {true, false}) {
    fake_esp8266::reset();
    fake_esp8266::deliver_on_delay = deliver_on_delay;
    EspTransport transport;
    open(transport);
    std::vector<uint8_t> expected(513 + 147);
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<uint8_t>((i * 73 + i / 256) & 255);
    }
    fake_esp8266::bytes.assign(expected.begin(), expected.begin() + 513);
    fake_esp8266::closing_tail.assign(expected.begin() + 513, expected.end());

    std::vector<char> received(expected.size() + 1);
    TEST_ASSERT_EQUAL_UINT(expected.size(), transport.input().readBytes(received.data(), received.size()));
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), received.data(), expected.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(core::ReadEnd::Closed), static_cast<int>(transport.read_end()));
    TEST_ASSERT_EQUAL_INT(-1, transport.input().read());
  }
}

void test_empty_closed_stream_and_live_idle_timeout_are_distinct() {
  EspTransport transport;
  open(transport);
  fake_esp8266::connected = false;
  TEST_ASSERT_EQUAL_INT(-1, transport.input().read());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::ReadEnd::Closed), static_cast<int>(transport.read_end()));
  TEST_ASSERT_EQUAL_UINT32(0, fake_esp8266::now);

  transport.close();
  fake_esp8266::reset();
  open(transport);
  TEST_ASSERT_EQUAL_INT(-1, transport.input().read());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::ReadEnd::Timeout), static_cast<int>(transport.read_end()));
  TEST_ASSERT_EQUAL_UINT32(5000, fake_esp8266::now);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_binary_stream_drains_closing_tail_before_eof);
  RUN_TEST(test_empty_closed_stream_and_live_idle_timeout_are_distinct);
  return UNITY_END();
}
