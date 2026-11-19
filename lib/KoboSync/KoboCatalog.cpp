#include "KoboCatalog.h"

#include <KoboSyncConfig.h>

#if CROSSPOINT_KOBO_SYNC

#include <Logging.h>
#include <Memory.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr char CATALOG_MAGIC[4] = {'K', 'B', 'K', '1'};
constexpr char MAP_MAGIC[4] = {'K', 'B', 'M', '1'};
constexpr uint16_t FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = 16;
constexpr size_t MAP_ENTRY_SIZE = kobo::CONTENT_ID_MAX + 4;  // id + uint32 offset
// Largest scratch array the map rebuild may allocate. 256 entries is ~11 KB;
// bigger catalogs keep working through the linear fallback instead of asking
// the C3 for an allocation proportional to the library size.
constexpr uint16_t MAX_SORT_ENTRIES = 256;
// FileHeader.reserved in catalog.map: 1 means the map is intentionally absent
// because this catalog is too large to sort within the RAM budget.
constexpr uint32_t MAP_LINEAR = 1;
// FileHeader.reserved in catalog.map: 2 means the sorted prefix has unsorted
// entries appended after it. A crash before finishSync() must not make the
// next open treat that tail as sorted.
constexpr uint32_t MAP_UNSORTED = 2;

struct FileHeader {
  char magic[4];
  uint16_t version;
  uint16_t recordSize;
  uint32_t count;
  uint32_t reserved;
};

static_assert(sizeof(FileHeader) == HEADER_SIZE, "catalog header layout");

bool validContentId(const char* id) {
  if (id == nullptr || strlen(id) != kobo::CONTENT_ID_MAX - 1) return false;
  for (size_t i = 0; i < kobo::CONTENT_ID_MAX - 1; i++) {
    const char c = id[i];
    const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
    if (dash) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool shelfContains(const char* shelf, const char* name) {
  if (shelf[0] == '\0' || name[0] == '\0') return false;
  const size_t nameLen = strlen(name);
  const char* cursor = shelf;
  while (*cursor) {
    const char* end = strstr(cursor, ", ");
    const size_t len = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
    if (len == nameLen && memcmp(cursor, name, len) == 0) return true;
    if (!end) break;
    cursor = end + 2;
  }
  return false;
}

}  // namespace

KoboCatalog::~KoboCatalog() { close(); }

bool KoboCatalog::ensureDirectory() {
  if (!Storage.exists(DIRECTORY) && !Storage.mkdir(DIRECTORY)) {
    LOG_ERR("KOBO", "Cannot create %s", DIRECTORY);
    return false;
  }
  if (!Storage.exists(LINKS_DIRECTORY) && !Storage.mkdir(LINKS_DIRECTORY)) {
    LOG_ERR("KOBO", "Cannot create %s", LINKS_DIRECTORY);
    return false;
  }
  return true;
}

bool KoboCatalog::open() {
  std::lock_guard<std::mutex> lock(mutex);
  if (open_) return true;
  if (!ensureDirectory()) return false;

  catalogFile = Storage.open(CATALOG_PATH, O_RDWR | O_CREAT);
  if (!catalogFile.isOpen()) {
    LOG_ERR("KOBO", "Cannot open catalog");
    return false;
  }
  mapFile = Storage.open(MAP_PATH, O_RDWR | O_CREAT);
  if (!mapFile.isOpen()) {
    LOG_ERR("KOBO", "Cannot open catalog map");
    catalogFile.close();
    return false;
  }

  FileHeader catalogHeader{};
  catalogFile.seek(0);
  const bool catalogOk = catalogFile.read(&catalogHeader, sizeof(catalogHeader)) == sizeof(catalogHeader) &&
                         memcmp(catalogHeader.magic, CATALOG_MAGIC, 4) == 0 &&
                         catalogHeader.version == FORMAT_VERSION && catalogHeader.recordSize == sizeof(KoboBookRecord);
  if (!catalogOk) {
    LOG_DBG("KOBO", "Creating catalog");
    resetDetected = true;
    // Path links reference content ids from the old catalog; they are stale
    // once the catalog is recreated.
    if (Storage.exists(LINKS_DIRECTORY)) Storage.removeDir(LINKS_DIRECTORY);
    catalogFile.truncate(0);
    // The old map describes rows that no longer exist.
    mapFile.truncate(0);
    catalogHeader = {};
    memcpy(catalogHeader.magic, CATALOG_MAGIC, 4);
    catalogHeader.version = FORMAT_VERSION;
    catalogHeader.recordSize = sizeof(KoboBookRecord);
    catalogFile.seek(0);
    if (catalogFile.write(&catalogHeader, sizeof(catalogHeader)) != sizeof(catalogHeader)) {
      LOG_ERR("KOBO", "Cannot write catalog header");
      catalogFile.close();
      mapFile.close();
      return false;
    }
    catalogCount = 0;
  } else {
    const uint32_t available =
        catalogFile.fileSize() > HEADER_SIZE
            ? static_cast<uint32_t>((catalogFile.fileSize() - HEADER_SIZE) / sizeof(KoboBookRecord))
            : 0;
    uint32_t count = catalogHeader.count < available ? catalogHeader.count : available;
    // Records are append-only and fixed-size, so any complete trailing rows are
    // real even when the header count is stale (a failed header write).
    // Recover them instead of showing an empty shelf; stop at the first row
    // whose content id is not intact, which is a partial write.
    if (count < available) {
      uint32_t recovered = count;
      for (uint32_t i = count; i < available && recovered < UINT16_MAX; i++) {
        KoboBookRecord record;
        if (!readRecordAt(HEADER_SIZE + i * sizeof(KoboBookRecord), record) || !validContentId(record.contentId)) {
          break;
        }
        recovered = i + 1;
        // A stale count on a large file can turn this into thousands of SD
        // reads; let the idle task run so the watchdog is not starved.
        if ((i & 63) == 63) vTaskDelay(1);
      }
      if (recovered != count) {
        LOG_DBG("KOBO", "Recovered %u catalog rows after header count %u", static_cast<unsigned>(recovered),
                static_cast<unsigned>(count));
        count = recovered;
      }
    }
    catalogCount = static_cast<uint16_t>(count);
    resetDetected = false;
    if (catalogCount != catalogHeader.count) {
      catalogHeader.count = catalogCount;
      catalogFile.seek(0);
      catalogFile.write(&catalogHeader, sizeof(catalogHeader));
      catalogFile.flush();
    }
  }

  FileHeader mapHeader{};
  mapFile.seek(0);
  const bool mapOk = mapFile.read(&mapHeader, sizeof(mapHeader)) == sizeof(mapHeader) &&
                     memcmp(mapHeader.magic, MAP_MAGIC, 4) == 0 && mapHeader.version == FORMAT_VERSION &&
                     mapHeader.recordSize == MAP_ENTRY_SIZE;
  mapLinear = mapOk && mapHeader.reserved == MAP_LINEAR;
  if (mapLinear) {
    // The previous run decided this catalog is too large for the bounded
    // sort. Lookups scan the catalog directly until it shrinks (it does not).
    mapCount = 0;
    mapValid = false;
  } else if (mapOk && mapHeader.reserved == 0) {
    const uint32_t available = mapFile.fileSize() > HEADER_SIZE
                                   ? static_cast<uint32_t>((mapFile.fileSize() - HEADER_SIZE) / MAP_ENTRY_SIZE)
                                   : 0;
    mapCount = static_cast<uint16_t>(mapHeader.count < available ? mapHeader.count : available);
    mapValid = mapCount == catalogCount;
  } else {
    mapCount = 0;
    mapValid = false;
  }
  if (mapLinear && catalogCount == 0) {
    // An emptied catalog no longer needs the linear fallback.
    mapLinear = false;
  }
  if (!mapValid && !mapLinear) {
    LOG_DBG("KOBO", "Rebuilding catalog map");
    rebuildMap();
  } else if (mapValid) {
    mapSortedCount = mapCount;
  }
  open_ = true;
  return true;
}

void KoboCatalog::close() {
  std::lock_guard<std::mutex> lock(mutex);
  if (catalogFile.isOpen()) catalogFile.close();
  if (mapFile.isOpen()) mapFile.close();
  open_ = false;
  catalogCount = 0;
  mapCount = 0;
  mapSortedCount = 0;
  mapValid = false;
  mapLinear = false;
  resetDetected = false;
}

bool KoboCatalog::writeCatalogHeader() {
  FileHeader header{};
  memcpy(header.magic, CATALOG_MAGIC, 4);
  header.version = FORMAT_VERSION;
  header.recordSize = sizeof(KoboBookRecord);
  header.count = catalogCount;
  const bool ok = catalogFile.seek(0) && catalogFile.write(&header, sizeof(header)) == sizeof(header);
  if (!ok) LOG_ERR("KOBO", "Cannot write catalog header");
  return ok;
}

bool KoboCatalog::writeMapHeader(const uint32_t reserved) {
  FileHeader header{};
  memcpy(header.magic, MAP_MAGIC, 4);
  header.version = FORMAT_VERSION;
  header.recordSize = MAP_ENTRY_SIZE;
  header.count = mapCount;
  header.reserved = reserved;
  return mapFile.seek(0) && mapFile.write(&header, sizeof(header)) == sizeof(header);
}

void KoboCatalog::invalidateMap() {
  mapFile.truncate(0);
  mapCount = 0;
  mapSortedCount = 0;
  mapValid = false;
  mapLinear = true;
  FileHeader header{};
  memcpy(header.magic, MAP_MAGIC, 4);
  header.version = FORMAT_VERSION;
  header.recordSize = MAP_ENTRY_SIZE;
  header.reserved = MAP_LINEAR;
  if (mapFile.seek(0)) mapFile.write(&header, sizeof(header));
}

bool KoboCatalog::readRecordAt(const uint32_t offset, KoboBookRecord& record) {
  return catalogFile.seek(offset) && catalogFile.read(&record, sizeof(record)) == sizeof(record);
}

bool KoboCatalog::writeRecordAt(const uint32_t offset, const KoboBookRecord& record) {
  return catalogFile.seek(offset) && catalogFile.write(&record, sizeof(record)) == sizeof(record);
}

bool KoboCatalog::readAt(const uint16_t index, KoboBookRecord& out) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_ || index >= catalogCount) return false;
  return readRecordAt(HEADER_SIZE + static_cast<uint32_t>(index) * sizeof(KoboBookRecord), out);
}

bool KoboCatalog::rebuildMap() {
  if (catalogCount > MAX_SORT_ENTRIES) {
    LOG_ERR("KOBO", "%u books exceeds the %u-entry sort budget; using linear lookup",
            static_cast<unsigned>(catalogCount), static_cast<unsigned>(MAX_SORT_ENTRIES));
    invalidateMap();
    return false;
  }
  mapFile.truncate(0);
  mapCount = 0;
  mapValid = false;
  mapLinear = false;
  if (!writeMapHeader()) {
    invalidateMap();
    return false;
  }
  if (catalogCount == 0) {
    mapSortedCount = 0;
    mapValid = true;
    return true;
  }
  auto entries = makeUniqueNoThrow<MapEntry[]>(catalogCount);
  if (!entries) {
    LOG_ERR("KOBO", "OOM rebuilding map for %u books; using linear lookup", static_cast<unsigned>(catalogCount));
    invalidateMap();
    return false;
  }
  for (uint16_t i = 0; i < catalogCount; i++) {
    KoboBookRecord record;
    if (!readRecordAt(HEADER_SIZE + static_cast<uint32_t>(i) * sizeof(KoboBookRecord), record)) {
      invalidateMap();
      return false;
    }
    memcpy(entries[i].contentId, record.contentId, sizeof(entries[i].contentId));
    entries[i].offset = HEADER_SIZE + static_cast<uint32_t>(i) * sizeof(KoboBookRecord);
  }
  // Insertion sort: recovery only, and the map is normally built by upsert().
  for (uint16_t i = 1; i < catalogCount; i++) {
    MapEntry current = entries[i];
    int j = i - 1;
    while (j >= 0 && strcmp(entries[j].contentId, current.contentId) > 0) {
      entries[j + 1] = entries[j];
      j--;
    }
    entries[j + 1] = current;
  }
  for (uint16_t i = 0; i < catalogCount; i++) {
    uint8_t raw[MAP_ENTRY_SIZE] = {};
    memcpy(raw, entries[i].contentId, kobo::CONTENT_ID_MAX);
    memcpy(raw + kobo::CONTENT_ID_MAX, &entries[i].offset, sizeof(entries[i].offset));
    mapFile.seek(HEADER_SIZE + static_cast<uint32_t>(i) * MAP_ENTRY_SIZE);
    if (mapFile.write(raw, sizeof(raw)) != sizeof(raw)) {
      invalidateMap();
      return false;
    }
  }
  mapCount = catalogCount;
  mapSortedCount = mapCount;
  mapValid = writeMapHeader();
  if (!mapValid) invalidateMap();
  return mapValid;
}

bool KoboCatalog::findMap(const char* contentId, uint16_t& index, uint32_t& offset) {
  if (!mapValid) {
    for (uint16_t i = 0; i < catalogCount; i++) {
      KoboBookRecord record;
      const uint32_t recordOffset = HEADER_SIZE + static_cast<uint32_t>(i) * sizeof(KoboBookRecord);
      if (!readRecordAt(recordOffset, record)) return false;
      if (strcmp(record.contentId, contentId) == 0) {
        index = i;
        offset = recordOffset;
        return true;
      }
    }
    return false;
  }
  uint16_t low = 0;
  uint16_t high = mapSortedCount;
  while (low < high) {
    const uint16_t mid = static_cast<uint16_t>((low + high) / 2);
    uint8_t raw[MAP_ENTRY_SIZE];
    mapFile.seek(HEADER_SIZE + static_cast<uint32_t>(mid) * MAP_ENTRY_SIZE);
    if (mapFile.read(raw, sizeof(raw)) != sizeof(raw) || !validMapEntry(raw)) return false;
    const int cmp = strcmp(contentId, reinterpret_cast<const char*>(raw));
    if (cmp == 0) {
      index = mid;
      memcpy(&offset, raw + kobo::CONTENT_ID_MAX, sizeof(offset));
      return true;
    }
    if (cmp < 0) {
      high = mid;
    } else {
      low = static_cast<uint16_t>(mid + 1);
    }
  }
  // Entries appended since the last finishSync() are not sorted yet.
  for (uint16_t i = mapSortedCount; i < mapCount; i++) {
    uint8_t raw[MAP_ENTRY_SIZE];
    mapFile.seek(HEADER_SIZE + static_cast<uint32_t>(i) * MAP_ENTRY_SIZE);
    if (mapFile.read(raw, sizeof(raw)) != sizeof(raw) || !validMapEntry(raw)) return false;
    if (strcmp(contentId, reinterpret_cast<const char*>(raw)) == 0) {
      index = i;
      memcpy(&offset, raw + kobo::CONTENT_ID_MAX, sizeof(offset));
      return true;
    }
  }
  return false;
}

bool KoboCatalog::insertMapEntry(const char* contentId, const uint32_t offset) {
  if (!mapValid || mapCount == UINT16_MAX) return false;
  // Append after the sorted prefix; finishSync() sorts the whole map once the
  // feed is done. This avoids an O(N^2) SD shift for a large first sync.
  const uint32_t entryOffset = HEADER_SIZE + static_cast<uint32_t>(mapCount) * MAP_ENTRY_SIZE;
  uint8_t raw[MAP_ENTRY_SIZE] = {};
  memcpy(raw, contentId, kobo::CONTENT_ID_MAX);
  memcpy(raw + kobo::CONTENT_ID_MAX, &offset, sizeof(offset));
  if (!mapFile.seek(entryOffset) || mapFile.write(raw, sizeof(raw)) != sizeof(raw)) {
    mapValid = false;
    return false;
  }
  mapCount++;
  if (!writeMapHeader(MAP_UNSORTED)) {
    mapValid = false;
    return false;
  }
  return true;
}

bool KoboCatalog::findByContentId(const char* contentId, KoboBookRecord& out, uint16_t* index) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_ || contentId == nullptr) return false;
  uint16_t foundIndex = 0;
  uint32_t offset = 0;
  if (!findMap(contentId, foundIndex, offset)) return false;
  if (!readRecordAt(offset, out)) return false;
  if (index) *index = foundIndex;
  return true;
}

bool KoboCatalog::upsert(const KoboBookRecord& record) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_ || !validContentId(record.contentId) || catalogCount == UINT16_MAX) return false;
  uint16_t index = 0;
  uint32_t offset = 0;
  if (findMap(record.contentId, index, offset)) {
    return writeRecordAt(offset, record);
  }
  offset = HEADER_SIZE + static_cast<uint32_t>(catalogCount) * sizeof(KoboBookRecord);
  if (!writeRecordAt(offset, record)) return false;
  catalogCount++;
  if (!writeCatalogHeader()) return false;
  if (!insertMapEntry(record.contentId, offset)) {
    if (!mapLinear) LOG_ERR("KOBO", "Map insert failed; lookups will scan");
    mapValid = false;
  }
  return true;
}

bool KoboCatalog::append(const KoboBookRecord& record) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_ || !validContentId(record.contentId) || catalogCount == UINT16_MAX) return false;
  const uint32_t offset = HEADER_SIZE + static_cast<uint32_t>(catalogCount) * sizeof(KoboBookRecord);
  if (!writeRecordAt(offset, record)) return false;
  catalogCount++;
  if (!writeCatalogHeader()) return false;
  if (!insertMapEntry(record.contentId, offset)) {
    if (!mapLinear) LOG_ERR("KOBO", "Map append failed; lookups will scan");
    mapValid = false;
  }
  return true;
}

void KoboCatalog::finishSync() {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_) return;
  // The catalog is the source of truth for the shelf; push its header and
  // records before the sync returns to the UI.
  catalogFile.flush();
  if (mapLinear) return;
  // Nothing was appended since the map was last sorted.
  if (mapValid && mapSortedCount == mapCount) return;
  // Sort the appended map tail once. On failure lookups stay correct but
  // linear, so a large catalog remains usable at a slower speed.
  if (!rebuildMap()) {
    LOG_ERR("KOBO", "Map rebuild failed; using linear lookup");
  } else {
    mapFile.flush();
  }
}

bool KoboCatalog::validMapEntry(const uint8_t* raw) {
  if (raw == nullptr || memchr(raw, '\0', kobo::CONTENT_ID_MAX) == nullptr) return false;
  return validContentId(reinterpret_cast<const char*>(raw));
}

bool KoboCatalog::removeAll() {
  bool ok = true;
  if (Storage.exists(CATALOG_PATH)) ok = Storage.remove(CATALOG_PATH) && ok;
  if (Storage.exists(MAP_PATH)) ok = Storage.remove(MAP_PATH) && ok;
  if (Storage.exists(LINKS_DIRECTORY)) ok = Storage.removeDir(LINKS_DIRECTORY) && ok;
  return ok;
}

bool KoboCatalog::clearNewFlags() {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_) return false;
  bool ok = true;
  for (uint16_t i = 0; i < catalogCount; i++) {
    const uint32_t offset = HEADER_SIZE + static_cast<uint32_t>(i) * sizeof(KoboBookRecord);
    KoboBookRecord record;
    if (!readRecordAt(offset, record)) return false;
    if ((record.flags & kobo_flags::NEW_SINCE_SYNC) == 0) continue;
    record.flags &= static_cast<uint8_t>(~kobo_flags::NEW_SINCE_SYNC);
    if (!writeRecordAt(offset, record)) ok = false;
  }
  return ok;
}

bool KoboCatalog::appendShelf(const char* contentId, const char* shelfName) {
  if (shelfName == nullptr || shelfName[0] == '\0') return true;
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_) return false;
  uint16_t index = 0;
  uint32_t offset = 0;
  if (!findMap(contentId, index, offset)) return true;  // shelf item for a book not in the catalog
  KoboBookRecord record;
  if (!readRecordAt(offset, record)) return false;
  if (shelfContains(record.shelf, shelfName)) return true;
  const size_t used = strlen(record.shelf);
  const size_t nameLen = strlen(shelfName);
  if (used > 0) {
    if (used + 2 + nameLen >= sizeof(record.shelf)) return true;
    memcpy(record.shelf + used, ", ", 2);
    memcpy(record.shelf + used + 2, shelfName, nameLen);
    record.shelf[used + 2 + nameLen] = '\0';
  } else {
    const size_t n = nameLen < sizeof(record.shelf) - 1 ? nameLen : sizeof(record.shelf) - 1;
    memcpy(record.shelf, shelfName, n);
    record.shelf[n] = '\0';
  }
  return writeRecordAt(offset, record);
}

bool KoboCatalog::setLocalPath(const char* contentId, const char* path) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_ || path == nullptr) return false;
  uint16_t index = 0;
  uint32_t offset = 0;
  if (!findMap(contentId, index, offset)) return false;
  KoboBookRecord record;
  if (!readRecordAt(offset, record)) return false;
  const size_t n = strlen(path);
  if (n >= sizeof(record.localPath)) return false;
  memcpy(record.localPath, path, n + 1);
  record.flags |= kobo_flags::DOWNLOADED;
  return writeRecordAt(offset, record);
}

bool KoboCatalog::markRemoved(const char* contentId, const bool removed) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!open_) return false;
  uint16_t index = 0;
  uint32_t offset = 0;
  if (!findMap(contentId, index, offset)) return true;
  KoboBookRecord record;
  if (!readRecordAt(offset, record)) return false;
  if (removed) {
    record.flags |= kobo_flags::REMOVED;
  } else {
    record.flags &= static_cast<uint8_t>(~kobo_flags::REMOVED);
  }
  return writeRecordAt(offset, record);
}

bool KoboCatalog::contentIdForPath(const std::string& path, std::string& out) {
  out.clear();
  char name[48];
  const size_t hash = std::hash<std::string>{}(path);
  snprintf(name, sizeof(name), "%s/%08lx.txt", LINKS_DIRECTORY, static_cast<unsigned long>(hash));
  // Most books have no link; treat a missing file as "not downloaded" without
  // asking the storage layer to log an open failure on every reader open.
  if (!Storage.exists(name)) return false;
  std::string content;
  if (!Storage.readFileToString("KOBO", name, 512, content)) return false;
  const size_t firstLine = content.find('\n');
  if (firstLine == std::string::npos || firstLine != kobo::CONTENT_ID_MAX - 1) return false;
  const std::string id = content.substr(0, firstLine);
  if (!validContentId(id.c_str())) return false;
  const size_t secondLine = content.find('\n', firstLine + 1);
  const std::string storedPath =
      content.substr(firstLine + 1, secondLine == std::string::npos ? std::string::npos : secondLine - firstLine - 1);
  if (storedPath != path) return false;
  out = id;
  return true;
}

bool KoboCatalog::setPathLink(const std::string& path, const std::string& contentId) {
  if (!validContentId(contentId.c_str())) return false;
  if (!Storage.exists(LINKS_DIRECTORY) && !Storage.mkdir(LINKS_DIRECTORY)) return false;
  char name[48];
  const size_t hash = std::hash<std::string>{}(path);
  snprintf(name, sizeof(name), "%s/%08lx.txt", LINKS_DIRECTORY, static_cast<unsigned long>(hash));
  std::string content;
  content.reserve(contentId.size() + path.size() + 2);
  content += contentId;
  content += '\n';
  content += path;
  content += '\n';
  HalFile file;
  if (!Storage.openFileForWrite("KOBO", name, file)) return false;
  const bool ok = file.write(content.data(), content.size()) == content.size();
  file.close();
  return ok;
}

#endif  // CROSSPOINT_KOBO_SYNC
