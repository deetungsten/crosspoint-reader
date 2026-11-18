#pragma once

#include <cstddef>
#include <cstdint>

// Bounded field sizes for one calibre-web Kobo sync entry. The feed can be
// arbitrarily large, so the parser copies only these fields and hands one
// entry at a time to the caller; nothing proportional to the feed is retained.
namespace kobo {

inline constexpr size_t CONTENT_ID_MAX = 37;  // UUID + NUL
inline constexpr size_t TITLE_MAX = 128;
inline constexpr size_t AUTHOR_MAX = 80;
inline constexpr size_t SERIES_MAX = 64;
inline constexpr size_t SHELF_MAX = 64;
inline constexpr size_t URL_MAX = 256;
inline constexpr size_t FORMAT_MAX = 16;
inline constexpr size_t TAG_NAME_MAX = 64;
inline constexpr size_t LOCATION_MAX = 96;
inline constexpr size_t LOCATION_TYPE_MAX = 32;

enum class ReadStatus : uint8_t { Unread = 0, Reading = 1, Finished = 2 };

enum class EntryKind : uint8_t { Book, ReadingState, Tag };

struct FeedEntry {
  EntryKind kind = EntryKind::Book;
  bool isNew = false;
  bool isRemoved = false;
  char contentId[CONTENT_ID_MAX] = {};
  char title[TITLE_MAX] = {};
  char author[AUTHOR_MAX] = {};
  char series[SERIES_MAX] = {};
  char downloadUrl[URL_MAX] = {};
  char downloadFormat[FORMAT_MAX] = {};
  uint32_t downloadSize = 0;
  uint16_t progressBp = 0;  // 0..10000, 0.01% units
  ReadStatus readStatus = ReadStatus::Unread;
  uint32_t readingStateLastModified = 0;
  char locationValue[LOCATION_MAX] = {};
  char locationType[LOCATION_TYPE_MAX] = {};
  // Tag entries: contentId is the tagged book's revision id.
  char tagId[CONTENT_ID_MAX] = {};
  char tagName[TAG_NAME_MAX] = {};
};

struct Metadata {
  char contentId[CONTENT_ID_MAX] = {};
  char title[TITLE_MAX] = {};
  char author[AUTHOR_MAX] = {};
  char downloadUrl[URL_MAX] = {};
  char downloadFormat[FORMAT_MAX] = {};
  uint32_t downloadSize = 0;
};

struct ReadingState {
  bool found = false;
  uint16_t progressBp = 0;
  ReadStatus readStatus = ReadStatus::Unread;
  uint32_t lastModified = 0;
  char locationValue[LOCATION_MAX] = {};
  char locationType[LOCATION_TYPE_MAX] = {};
};

}  // namespace kobo
