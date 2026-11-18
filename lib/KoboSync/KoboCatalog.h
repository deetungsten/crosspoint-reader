#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <HalStorage.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "KoboSyncTypes.h"

// One catalog row. Fixed size so the sync merge can update a record in place
// and the shelf screen can read one row without loading the catalog.
struct KoboBookRecord {
  uint32_t lastModified = 0;
  uint16_t progressBp = 0;
  uint8_t flags = 0;
  uint8_t readStatus = 0;
  uint8_t reserved[8] = {};
  char contentId[kobo::CONTENT_ID_MAX] = {};
  char title[kobo::TITLE_MAX] = {};
  char author[kobo::AUTHOR_MAX] = {};
  char series[kobo::SERIES_MAX] = {};
  char shelf[kobo::SHELF_MAX] = {};
  char localPath[192] = {};
};

namespace kobo_flags {
inline constexpr uint8_t NEW_SINCE_SYNC = 1u << 0;
inline constexpr uint8_t DOWNLOADED = 1u << 1;
inline constexpr uint8_t REMOVED = 1u << 2;
}  // namespace kobo_flags

// Persistent index of books seen in the calibre-web Kobo feed plus the
// local-file mapping used to attach reading status to an opened book.
//
// Storage layout under /.crosspoint/kobo/:
//   catalog.bin  header + fixed KoboBookRecord rows (append order)
//   catalog.map  header + sorted {contentId, offset} entries for O(log n) lookup
//   links/<hash> contentId + local path for O(1) path -> contentId lookup
class KoboCatalog {
 public:
  static constexpr const char* DIRECTORY = "/.crosspoint/kobo";
  static constexpr const char* CATALOG_PATH = "/.crosspoint/kobo/catalog.bin";
  static constexpr const char* MAP_PATH = "/.crosspoint/kobo/catalog.map";
  static constexpr const char* LINKS_DIRECTORY = "/.crosspoint/kobo/links";

  KoboCatalog() = default;
  ~KoboCatalog();
  KoboCatalog(const KoboCatalog&) = delete;
  KoboCatalog& operator=(const KoboCatalog&) = delete;

  bool open();
  void close();
  bool isOpen() const { return open_; }
  uint16_t count() const { return catalogCount; }

  bool readAt(uint16_t index, KoboBookRecord& out);
  bool findByContentId(const char* contentId, KoboBookRecord& out, uint16_t* index = nullptr);
  bool upsert(const KoboBookRecord& record);
  // Append a record known to be new (first-sync fast path): no map search and
  // no sorted-map shift. finishSync() sorts the map once after the feed.
  bool append(const KoboBookRecord& record);
  void finishSync();
  // True when open() had to recreate the catalog (format bump or corruption).
  // The caller must drop the sync token so the server re-sends the library.
  bool wasReset() const { return resetDetected; }
  void clearResetFlag() { resetDetected = false; }
  // Removes the catalog, map, and all path links. Used when the server/user
  // identity changes; safe to call while closed.
  static bool removeAll();
  // Clears NEW_SINCE_SYNC on every row before a sync page set is applied.
  bool clearNewFlags();
  bool appendShelf(const char* contentId, const char* shelfName);
  bool setLocalPath(const char* contentId, const char* path);
  bool markRemoved(const char* contentId, bool removed);

  // Path -> content id link used by the reader. The link file also stores the
  // path so a std::hash collision cannot attach progress to the wrong book.
  static bool contentIdForPath(const std::string& path, std::string& out);
  static bool setPathLink(const std::string& path, const std::string& contentId);

 private:
  struct MapEntry {
    char contentId[kobo::CONTENT_ID_MAX] = {};
    uint32_t offset = 0;
  };

  bool ensureDirectory();
  bool readHeader();
  bool writeCatalogHeader();
  bool writeMapHeader(uint32_t reserved = 0);
  bool rebuildMap();
  // Drops the map and records "linear lookup" in its header so a catalog too
  // large for the bounded sort does not retry the allocation on every open.
  void invalidateMap();
  bool findMap(const char* contentId, uint16_t& index, uint32_t& offset);
  bool insertMapEntry(const char* contentId, uint32_t offset);
  static bool validMapEntry(const uint8_t* raw);
  bool writeRecordAt(uint32_t offset, const KoboBookRecord& record);
  bool readRecordAt(uint32_t offset, KoboBookRecord& record);

  mutable std::mutex mutex;
  HalFile catalogFile;
  HalFile mapFile;
  bool open_ = false;
  bool mapValid = false;
  bool mapLinear = false;
  bool resetDetected = false;
  uint16_t catalogCount = 0;
  uint16_t mapCount = 0;
  // New map entries are appended after this sorted prefix and sorted by
  // finishSync(); lookups scan the short unsorted tail linearly.
  uint16_t mapSortedCount = 0;
};

#endif  // CROSSPOINT_KOBO_SYNC
