#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

#include "KoboSyncTypes.h"

// Streaming parser for calibre-web's `/v1/library/sync` response: a JSON array
// of entitlements, reading states and tags. It keeps one bounded FeedEntry and
// emits it as soon as the containing object closes, so a large first sync is
// never buffered.
class KoboFeedParser {
 public:
  using EntryCallback = void (*)(void* ctx, const kobo::FeedEntry& entry);

  KoboFeedParser(EntryCallback callback, void* context);
  KoboFeedParser(const KoboFeedParser&) = delete;
  KoboFeedParser& operator=(const KoboFeedParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);
  bool finished() const { return done; }
  bool hasError() const { return parser.hasError(); }
  uint32_t entryCount() const { return entries; }

 private:
  enum class Ctx : uint8_t {
    None,
    TopArray,
    // calibre-web wraps every item: {"NewEntitlement": {...}}. The wrapper
    // names the entry kind; its single child object holds the payload.
    Wrapper,
    Entry,
    BookEntitlement,
    BookMetadata,
    ReadingState,
    Bookmark,
    Location,
    StatusInfo,
    Statistics,
    Series,
    DownloadUrls,
    DownloadUrl,
    Contributors,
    Tag,
    TagItems,
    TagItem,
    Other,
  };

  struct Frame {
    Ctx ctx = Ctx::None;
    char key[40] = {};
  };

  static constexpr uint8_t MAX_DEPTH = 16;

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void setKey(const char* key, size_t len);
  bool keyIs(const char* key) const;
  Ctx childContext(Ctx parent, const char* key) const;
  void push(Ctx ctx);
  void pop();
  void beginEntry(const char* key, size_t len);
  void emitEntry();
  void emitTagItems();
  void appendAuthor(const char* value, size_t len);

  StreamingJsonParser parser;
  EntryCallback callback;
  void* callbackContext;
  Frame frames[MAX_DEPTH];
  uint8_t depth = 0;
  kobo::FeedEntry entry;
  char pendingUrl[kobo::URL_MAX] = {};
  char pendingFormat[kobo::FORMAT_MAX] = {};
  uint32_t pendingSize = 0;
  char itemId[kobo::CONTENT_ID_MAX] = {};
  // calibre-web serializes a Tag's Items before its Name. Buffer the bounded
  // item list until the Tag closes so appendShelf() sees the name.
  static constexpr uint8_t MAX_TAG_ITEMS = 32;
  char tagItemIds[MAX_TAG_ITEMS][kobo::CONTENT_ID_MAX] = {};
  uint8_t tagItemCount = 0;
  uint32_t entries = 0;
  bool progressSet = false;
  bool done = false;
};

#endif  // CROSSPOINT_KOBO_SYNC
