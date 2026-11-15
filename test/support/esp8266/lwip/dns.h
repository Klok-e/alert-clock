#pragma once
#include "tcp.h"
inline constexpr uint8_t LWIP_DNS_ADDRTYPE_IPV4 = 0;
using dns_found_callback = void (*)(const char*, const ip_addr_t*, void*);
namespace fake_lwip {
struct DnsQuery { dns_found_callback callback; void* argument; };
inline std::vector<DnsQuery> dns;
inline err_t dns_result = ERR_INPROGRESS;
inline void answer_dns(size_t index, bool success = true) {
  const auto query = dns.at(index);
  dns[index].callback = nullptr;
  const ip_addr_t address{123};
  query.callback("gateway.test", success ? &address : nullptr, query.argument);
}
inline void reset_dns() {
  for (size_t i = 0; i < dns.size(); ++i) if (dns[i].callback != nullptr) answer_dns(i, false);
  dns.clear(); dns_result = ERR_INPROGRESS;
}
}
inline err_t dns_gethostbyname_addrtype(const char*, ip_addr_t* address,
                                      dns_found_callback callback, void* argument, uint8_t) {
  if (fake_lwip::dns_result == ERR_OK) address->addr = 123;
  if (fake_lwip::dns_result == ERR_INPROGRESS) fake_lwip::dns.push_back({callback, argument});
  return fake_lwip::dns_result;
}
