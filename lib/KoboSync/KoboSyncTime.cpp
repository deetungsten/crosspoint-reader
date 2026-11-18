#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <cstdio>

#include "KoboSyncTime.h"

namespace kobo {

namespace {
int64_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}
}  // namespace

uint32_t parseIso8601Utc(const char* value) {
  if (value == nullptr) return 0;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) return 0;
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
  const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
  return seconds > 0 && seconds < 0xFFFFFFFFLL ? static_cast<uint32_t>(seconds) : 0;
}

}  // namespace kobo

#endif  // CROSSPOINT_KOBO_SYNC
