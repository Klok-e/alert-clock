#include "esp_platform.h"

#include <time.h>

namespace {

// Anything earlier means SNTP has not answered yet (2023-11-14).
constexpr time_t kMinValidUnix = 1700000000;

}  // namespace

uint32_t EspTime::millis() { return ::millis(); }

int64_t EspTime::unix_time() {
  const time_t now = time(nullptr);
  return now < kMinValidUnix ? 0 : static_cast<int64_t>(now);
}

bool EspTime::to_local(int64_t unix_time, core::LocalTime& out) {
  const time_t t = static_cast<time_t>(unix_time);
  struct tm local {};
  if (localtime_r(&t, &local) == nullptr) {
    return false;
  }
  out.year = local.tm_year + 1900;
  out.month = static_cast<uint8_t>(local.tm_mon + 1);
  out.day = static_cast<uint8_t>(local.tm_mday);
  out.hour = static_cast<uint8_t>(local.tm_hour);
  out.minute = static_cast<uint8_t>(local.tm_min);
  out.second = static_cast<uint8_t>(local.tm_sec);
  out.weekday = static_cast<uint8_t>(local.tm_wday);
  return true;
}

EspTransport::DnsTicket EspTransport::dns_tickets_[3];

EspTransport::~EspTransport() {
  close();
  for (auto& ticket : dns_tickets_) {
    if (ticket.owner == this) ticket.owner = nullptr;
  }
}

bool EspTransport::begin(const core::Url& url, uint32_t now_ms) {
  (void)now_ms;
  close();
  read_end_ = core::ReadEnd::None;
  if (url.tls || WiFi.status() != WL_CONNECTED) return false;
  port_ = url.port;
  state_ = core::LinkState::Pending;
  if (ipaddr_aton(url.host.c_str(), &address_)) {
    address_ready_ = true;
    return true;
  }
  for (auto& ticket : dns_tickets_) {
    if (ticket.occupied) continue;
    ticket.owner = this;
    ticket.generation = generation_;
    ticket.occupied = true;
    const err_t result = dns_gethostbyname_addrtype(url.host.c_str(), &address_, dns_found,
                                                   &ticket, LWIP_DNS_ADDRTYPE_IPV4);
    if (result == ERR_INPROGRESS) return true;
    ticket.occupied = false;
    ticket.owner = nullptr;
    address_ready_ = result == ERR_OK;
    if (!address_ready_) state_ = core::LinkState::Failed;
    return address_ready_;
  }
  state_ = core::LinkState::Failed;
  return false;
}

void EspTransport::dns_found(const char*, const ip_addr_t* address, void* argument) {
  auto& ticket = *static_cast<DnsTicket*>(argument);
  EspTransport* owner = ticket.owner;
  if (owner != nullptr && owner->generation_ == ticket.generation &&
      owner->state_ == core::LinkState::Pending) {
    if (address != nullptr) {
      owner->address_ = *address;
      owner->address_ready_ = true;
    } else {
      owner->state_ = core::LinkState::Failed;
    }
  }
  ticket.occupied = false;
  ticket.owner = nullptr;
}

core::LinkState EspTransport::advance(uint32_t now_ms) {
  (void)now_ms;
  if (state_ != core::LinkState::Pending || !address_ready_ || pcb_ != nullptr) return state_;
  pcb_ = tcp_new();
  if (pcb_ == nullptr) {
    state_ = core::LinkState::Failed;
    return state_;
  }
  tcp_arg(pcb_, this);
  tcp_recv(pcb_, received);
  tcp_err(pcb_, failed);
  const err_t result = tcp_connect(pcb_, &address_, port_, connected);
  if (result != ERR_OK) {
    detach_and_close(true);
    state_ = core::LinkState::Failed;
  }
  return state_;
}

err_t EspTransport::connected(void* argument, tcp_pcb* pcb, err_t error) {
  auto& self = *static_cast<EspTransport*>(argument);
  if (self.pcb_ == pcb && error == ERR_OK) self.state_ = core::LinkState::Ready;
  return ERR_OK;
}

err_t EspTransport::received(void* argument, tcp_pcb* pcb, pbuf* data, err_t error) {
  auto& self = *static_cast<EspTransport*>(argument);
  if (self.pcb_ != pcb || error != ERR_OK) return ERR_MEM;
  if (data == nullptr) {
    self.eof_ = true;
    return ERR_OK;
  }
  if (data->tot_len == 0) {
    pbuf_free(data);
    return ERR_OK;
  }
  if (self.received_ != nullptr) return ERR_MEM;
  self.received_ = data;
  self.offset_ = 0;
  return ERR_OK;
}

void EspTransport::failed(void* argument, err_t error) {
  (void)error;
  auto& self = *static_cast<EspTransport*>(argument);
  // lwIP has freed the PCB before invoking tcp_err.
  self.pcb_ = nullptr;
  self.read_end_ = core::ReadEnd::Error;
  self.state_ = core::LinkState::Failed;
}

core::IoResult EspTransport::write_some(const uint8_t* data, size_t length) {
  if (state_ == core::LinkState::Failed) return {core::IoState::Failed, 0};
  if (state_ != core::LinkState::Ready || length == 0) return {core::IoState::Pending, 0};
  const size_t room = tcp_sndbuf(pcb_);
  const size_t available = length < room ? length : room;
  const size_t count = available < 256 ? available : 256;
  if (count == 0) return {core::IoState::Pending, 0};
  const err_t result = tcp_write(pcb_, data, static_cast<uint16_t>(count), TCP_WRITE_FLAG_COPY);
  if (result == ERR_MEM) return {core::IoState::Pending, 0};
  if (result != ERR_OK) {
    read_end_ = core::ReadEnd::Error;
    state_ = core::LinkState::Failed;
    return {core::IoState::Failed, 0};
  }
  tcp_output(pcb_);
  return {core::IoState::Progress, count};
}

core::IoResult EspTransport::read_some(uint8_t* data, size_t capacity) {
  if (capacity == 0) return {core::IoState::Pending, 0};
  if (received_ != nullptr && capacity > 0) {
    const size_t left = received_->tot_len - offset_;
    const size_t available = capacity < left ? capacity : left;
    const size_t count = available < 256 ? available : 256;
    const size_t copied = pbuf_copy_partial(received_, data, static_cast<uint16_t>(count),
                                            static_cast<uint16_t>(offset_));
    offset_ += copied;
    if (pcb_ != nullptr) tcp_recved(pcb_, static_cast<uint16_t>(copied));
    if (offset_ == received_->tot_len) {
      pbuf_free(received_);
      received_ = nullptr;
      offset_ = 0;
    }
    return {core::IoState::Progress, copied};
  }
  if (state_ == core::LinkState::Failed) return {core::IoState::Failed, 0};
  if (eof_) {
    read_end_ = core::ReadEnd::Closed;
    return {core::IoState::End, 0};
  }
  return {core::IoState::Pending, 0};
}

void EspTransport::detach_and_close(bool abort_connection) {
  if (pcb_ == nullptr) return;
  tcp_pcb* pcb = pcb_;
  pcb_ = nullptr;
  tcp_arg(pcb, nullptr);
  tcp_recv(pcb, nullptr);
  tcp_sent(pcb, nullptr);
  tcp_err(pcb, nullptr);
  tcp_poll(pcb, nullptr, 0);
  if (abort_connection || tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
}

void EspTransport::close() {
  ++generation_;
  detach_and_close(state_ != core::LinkState::Ready || received_ != nullptr);
  if (received_ != nullptr) pbuf_free(received_);
  received_ = nullptr;
  offset_ = 0;
  address_ready_ = false;
  eof_ = false;
  state_ = core::LinkState::Failed;
}

uint32_t EspRandom::next() { return RANDOM_REG32; }
