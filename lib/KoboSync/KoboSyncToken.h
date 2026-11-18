#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <cstdint>
#include <string>

// calibre-web's Kobo sync token (cps/SyncToken.py): a base64-encoded JSON
// object carrying the six "last seen" timestamps. CrossPoint replays the token
// verbatim; decoding it is used only to validate a persisted token and to
// expose its timestamps for diagnostics and tests.
struct KoboSyncToken {
  static constexpr size_t MAX_ENCODED_LEN = 2048;
  static constexpr size_t MAX_RAW_STORE_TOKEN = 768;

  std::string encoded;  // token as received/sent
  std::string rawKoboStoreToken;
  double booksLastCreated = 0;
  double booksLastModified = 0;
  double archiveLastModified = 0;
  double readingStateLastModified = 0;
  double tagsLastModified = 0;
  bool decoded = false;  // true when the JSON fields were parsed

  bool empty() const { return encoded.empty(); }

  // Parse a header value. A token from the official Kobo store (two base64
  // blobs separated by '.') is kept verbatim and marked not decoded.
  static KoboSyncToken fromHeader(const std::string& value);

  // Build the calibre-web token from the current fields.
  std::string build() const;

  // True when this is a plausible calibre-web token (decoded, or an official
  // store token with a dot).
  bool valid() const;
};

namespace kobo_base64 {

std::string encode(const std::string& input);
bool decode(const std::string& input, std::string& out);

}  // namespace kobo_base64

#endif  // CROSSPOINT_KOBO_SYNC
