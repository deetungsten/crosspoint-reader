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

// A book counts as finished from 99.5%: the last laid-out page computes just
// under 10000 bp, so the threshold sits below it, while rounding noise between
// layout engines must not mark a book read. Shared by the close-time queue and
// the conflict activity so both upload the same status for the same position.
inline constexpr uint16_t FINISHED_BASIS_POINTS = 9950;

inline ReadStatus readStatusFor(const uint16_t progressBp) {
  if (progressBp >= FINISHED_BASIS_POINTS) return ReadStatus::Finished;
  return progressBp > 0 ? ReadStatus::Reading : ReadStatus::Unread;
}

enum class SyncAction : uint8_t { ApplyRemote, UploadLocal, AlreadySynced };

// SMART conflict resolution: whichever side is further along by more than
// `toleranceBp` wins; anything smaller is rounding noise between engines.
inline SyncAction syncActionFor(const uint16_t remoteBp, const uint16_t localBp, const uint16_t toleranceBp) {
  const int remoteWithTolerance = remoteBp + toleranceBp;
  const int localWithTolerance = localBp + toleranceBp;
  if (remoteBp > localWithTolerance) return SyncAction::ApplyRemote;
  if (localBp > remoteWithTolerance) return SyncAction::UploadLocal;
  return SyncAction::AlreadySynced;
}

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
