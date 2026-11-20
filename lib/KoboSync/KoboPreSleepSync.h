#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <cctype>
#include <cstdint>
#include <string>

// Pure decisions for the timeout-triggered sync that runs just before deep
// sleep. Kept free of Arduino/Wi-Fi types so the host tests can exercise the
// policy and the 10-second budget without a radio.
namespace kobo {

// The probe is a cheap reachability check, not part of the sync itself.
inline constexpr uint32_t PRE_SLEEP_PROBE_BUDGET_MS = 10000;

inline bool shouldAttemptPreSleepSync(const bool fromTimeout, const bool configured, const bool wifiConnected) {
  return fromTimeout && configured && wifiConnected;
}

// Wrap-safe elapsed comparison: millis() rolls over every ~49 days.
inline bool probeBudgetExpired(const uint32_t startMs, const uint32_t nowMs, const uint32_t budgetMs) {
  return static_cast<uint32_t>(nowMs - startMs) >= budgetMs;
}

inline uint32_t remainingProbeBudgetMs(const uint32_t startMs, const uint32_t nowMs, const uint32_t budgetMs) {
  const uint32_t elapsed = static_cast<uint32_t>(nowMs - startMs);
  return elapsed >= budgetMs ? 0 : budgetMs - elapsed;
}

struct EndpointAddress {
  std::string host;
  uint16_t port = 0;
  bool secure = false;
  bool valid = false;
};

// Splits a base URL into the host and port a TCP reachability probe needs.
// Accepts http(s)://host[:port][/path], userinfo, and bracketed IPv6.
inline EndpointAddress parseEndpointAddress(const std::string& url) {
  EndpointAddress out;
  std::string rest = url;
  if (rest.rfind("https://", 0) == 0) {
    out.secure = true;
    out.port = 443;
    rest.erase(0, 8);
  } else if (rest.rfind("http://", 0) == 0) {
    out.port = 80;
    rest.erase(0, 7);
  } else {
    return out;
  }

  const size_t authorityEnd = rest.find_first_of("/?#");
  std::string authority = rest.substr(0, authorityEnd);
  const size_t userinfo = authority.rfind('@');
  if (userinfo != std::string::npos) authority.erase(0, userinfo + 1);
  if (authority.empty()) return out;

  std::string portText;
  if (authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos || close == 1) return out;
    out.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') return out;
      portText = authority.substr(close + 2);
    }
  } else {
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
      if (authority.find(':', colon + 1) != std::string::npos) return out;  // unbracketed IPv6
      out.host = authority.substr(0, colon);
      portText = authority.substr(colon + 1);
    } else {
      out.host = authority;
    }
  }

  if (out.host.empty()) return out;
  if (!portText.empty()) {
    uint32_t port = 0;
    for (const char c : portText) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return out;
      port = port * 10 + static_cast<uint32_t>(c - '0');
      if (port > 65535) return out;
    }
    if (port == 0) return out;
    out.port = static_cast<uint16_t>(port);
  }

  out.valid = true;
  return out;
}

}  // namespace kobo

#endif  // CROSSPOINT_KOBO_SYNC
