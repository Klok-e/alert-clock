#include <unity.h>

#include "../../src/device/esp_platform.cpp"

void setUp() {
  fake_lwip::reset_dns();
  fake_lwip::reset();
  fake_esp8266::reset();
}
void tearDown() { fake_lwip::reset_dns(); fake_lwip::reset(); }

static core::Url url(const char* host = "192.0.2.1") {
  core::Url result;
  result.host = host;
  result.port = 8080;
  return result;
}
static void open(EspTransport& transport) {
  TEST_ASSERT_TRUE(transport.begin(url(), 0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::LinkState::Pending), static_cast<int>(transport.advance(0)));
  fake_lwip::establish();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::LinkState::Ready), static_cast<int>(transport.advance(1)));
}
static void io(core::IoState expected, const core::IoResult& result, size_t count = 0) {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result.state));
  TEST_ASSERT_EQUAL_UINT(count, result.count);
}

void test_dns_cancelled_results_cannot_connect_another_operation() {
  EspTransport transport;
  TEST_ASSERT_TRUE(transport.begin(url("old.test"), 0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::LinkState::Pending), static_cast<int>(transport.advance(1)));
  TEST_ASSERT_TRUE(fake_lwip::pcbs.empty());
  transport.close();
  TEST_ASSERT_TRUE(transport.begin(url("new.test"), 2));
  fake_lwip::answer_dns(0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::LinkState::Pending), static_cast<int>(transport.advance(3)));
  TEST_ASSERT_TRUE(fake_lwip::pcbs.empty());
  fake_lwip::answer_dns(1);
  transport.advance(4);
  TEST_ASSERT_EQUAL_UINT(1, fake_lwip::pcbs.size());
  fake_lwip::establish();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::LinkState::Ready), static_cast<int>(transport.advance(5)));
}

void test_dns_tickets_are_bounded_and_survive_owner_destruction() {
  {
    EspTransport transport;
    for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(transport.begin(url("lost.test"), 0));
    TEST_ASSERT_FALSE(transport.begin(url("fourth.test"), 0));
    TEST_ASSERT_EQUAL_UINT(3, fake_lwip::dns.size());
    open(transport);  // Numeric addresses still work when all DNS tickets are occupied.
  }
  for (size_t i = 0; i < 3; ++i) fake_lwip::answer_dns(i);
  EspTransport next;
  TEST_ASSERT_TRUE(next.begin(url("next.test"), 0));
  fake_lwip::answer_dns(3, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::LinkState::Failed), static_cast<int>(next.advance(1)));
  TEST_ASSERT_TRUE(fake_lwip::pcbs.size() == 1);
}

void test_receive_backpressure_preserves_chain_and_fin_tail() {
  EspTransport transport;
  open(transport);
  uint8_t bytes[8]{};
  io(core::IoState::Pending, transport.read_some(bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL_UINT32(0, fake_esp8266::now);
  TEST_ASSERT_EQUAL_INT(ERR_OK, fake_lwip::deliver(fake_lwip::packet({1, 2}, fake_lwip::packet({3, 4, 5}))));
  auto* refused = fake_lwip::packet({6, 7, 8});
  TEST_ASSERT_EQUAL_INT(ERR_MEM, fake_lwip::deliver(refused));
  TEST_ASSERT_EQUAL_UINT(0, fake_lwip::freed_pbufs);
  io(core::IoState::Progress, transport.read_some(bytes, 3), 3);
  const uint8_t first[] = {1, 2, 3};
  TEST_ASSERT_EQUAL_MEMORY(first, bytes, 3);
  TEST_ASSERT_EQUAL_UINT(3, fake_lwip::credited);
  io(core::IoState::Progress, transport.read_some(bytes, sizeof(bytes)), 2);
  TEST_ASSERT_EQUAL_UINT8(4, bytes[0]);
  TEST_ASSERT_EQUAL_UINT8(5, bytes[1]);
  TEST_ASSERT_EQUAL_UINT(2, fake_lwip::freed_pbufs);
  TEST_ASSERT_EQUAL_INT(ERR_OK, fake_lwip::deliver(refused));
  TEST_ASSERT_EQUAL_INT(ERR_OK, fake_lwip::deliver(nullptr));
  io(core::IoState::Progress, transport.read_some(bytes, sizeof(bytes)), 3);
  const uint8_t tail[] = {6, 7, 8};
  TEST_ASSERT_EQUAL_MEMORY(tail, bytes, 3);
  io(core::IoState::End, transport.read_some(bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::ReadEnd::Closed), static_cast<int>(transport.read_end()));
  TEST_ASSERT_EQUAL_UINT(8, fake_lwip::credited);
  TEST_ASSERT_EQUAL_UINT(3, fake_lwip::freed_pbufs);
}

void test_reset_preserves_accepted_bytes_without_touching_freed_pcb() {
  EspTransport transport;
  open(transport);
  TEST_ASSERT_EQUAL_INT(ERR_OK, fake_lwip::deliver(fake_lwip::packet({42, 43})));
  fake_lwip::reset_peer();
  uint8_t bytes[8]{};
  io(core::IoState::Progress, transport.read_some(bytes, sizeof(bytes)), 2);
  TEST_ASSERT_EQUAL_UINT8(42, bytes[0]);
  io(core::IoState::Failed, transport.read_some(bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL_UINT(0, fake_lwip::credited);
  TEST_ASSERT_EQUAL_UINT(1, fake_lwip::freed_pbufs);
  transport.close();
  transport.close();
  TEST_ASSERT_EQUAL_UINT(0, fake_lwip::aborted);
}

void test_partial_writes_copy_bytes_and_retry_without_waiting() {
  EspTransport transport;
  open(transport);
  uint8_t request[] = {9, 8, 7, 6};
  fake_lwip::send_room = 0;
  io(core::IoState::Pending, transport.write_some(request, sizeof(request)));
  fake_lwip::send_room = 2;
  fake_lwip::write_result = ERR_MEM;
  io(core::IoState::Pending, transport.write_some(request, sizeof(request)));
  TEST_ASSERT_TRUE(fake_lwip::sent.empty());
  fake_lwip::write_result = ERR_OK;
  io(core::IoState::Progress, transport.write_some(request, sizeof(request)), 2);
  request[0] = 0;
  TEST_ASSERT_EQUAL_UINT8(9, fake_lwip::sent[0]);
  io(core::IoState::Progress, transport.write_some(request + 2, 2), 2);
  const uint8_t expected[] = {9, 8, 7, 6};
  TEST_ASSERT_EQUAL_MEMORY(expected, fake_lwip::sent.data(), sizeof(expected));
  uint8_t large[600]{};
  fake_lwip::send_room = 1024;
  io(core::IoState::Progress, transport.write_some(large, sizeof(large)), 256);
  TEST_ASSERT_EQUAL_UINT32(0, fake_esp8266::now);
}

void test_close_cancels_connect_and_frees_unread_data_once() {
  EspTransport transport;
  TEST_ASSERT_TRUE(transport.begin(url(), 0));
  transport.advance(0);
  transport.close();
  TEST_ASSERT_EQUAL_UINT(1, fake_lwip::aborted);
  open(transport);
  TEST_ASSERT_EQUAL_INT(ERR_OK, fake_lwip::deliver(fake_lwip::packet({1})));
  transport.close();
  transport.close();
  TEST_ASSERT_EQUAL_UINT(2, fake_lwip::aborted);
  TEST_ASSERT_EQUAL_UINT(1, fake_lwip::freed_pbufs);
  open(transport);
  fake_lwip::close_result = ERR_MEM;
  transport.close();
  TEST_ASSERT_EQUAL_UINT(1, fake_lwip::closed);
  TEST_ASSERT_EQUAL_UINT(3, fake_lwip::aborted);
}

void test_connection_failures_return_without_retrying_in_the_adapter() {
  EspTransport transport;
  fake_lwip::allocation_fails = true;
  TEST_ASSERT_TRUE(transport.begin(url(), 0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::LinkState::Failed), static_cast<int>(transport.advance(0)));
  fake_lwip::allocation_fails = false;
  fake_lwip::connect_result = ERR_RST;
  TEST_ASSERT_TRUE(transport.begin(url(), 0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(core::LinkState::Failed), static_cast<int>(transport.advance(0)));
  TEST_ASSERT_EQUAL_UINT(1, fake_lwip::aborted);
  fake_esp8266::wifi_up = false;
  TEST_ASSERT_FALSE(transport.begin(url(), 0));
  TEST_ASSERT_EQUAL_UINT32(0, fake_esp8266::now);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_dns_cancelled_results_cannot_connect_another_operation);
  RUN_TEST(test_dns_tickets_are_bounded_and_survive_owner_destruction);
  RUN_TEST(test_receive_backpressure_preserves_chain_and_fin_tail);
  RUN_TEST(test_reset_preserves_accepted_bytes_without_touching_freed_pcb);
  RUN_TEST(test_partial_writes_copy_bytes_and_retry_without_waiting);
  RUN_TEST(test_close_cancels_connect_and_frees_unread_data_once);
  RUN_TEST(test_connection_failures_return_without_retrying_in_the_adapter);
  return UNITY_END();
}
