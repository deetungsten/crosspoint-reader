#pragma once

namespace kobo {

// A catalog row needs a transfer unless it was removed or its recorded local
// file is still on SD. The caller re-checks the file at download time because
// the catalog mapping can be stale.
inline bool needsDownload(const bool removed, const bool hasLocalPath, const bool localFileExists) {
  return !removed && !(hasLocalPath && localFileExists);
}

}  // namespace kobo
