#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <cstdint>

namespace kobo {

// Parse calibre-web's "YYYY-MM-DDTHH:MM:SSZ" timestamps to a UTC epoch.
// Returns 0 for malformed or pre-1970 values.
uint32_t parseIso8601Utc(const char* value);

}  // namespace kobo

#endif  // CROSSPOINT_KOBO_SYNC
