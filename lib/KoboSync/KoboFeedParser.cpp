#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "KoboFeedParser.h"
#include "KoboText.h"
#include "KoboSyncTime.h"

namespace {

void copyField(char* dst, const size_t cap, const char* src, const size_t len) {
  if (cap == 0) return;
  size_t n = len < cap - 1 ? len : cap - 1;
  // Do not split a UTF-8 codepoint at the truncation boundary. src has only
  // `len` valid bytes, so inspect src[n] only when the field was truncated.
  if (n < len) {
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

bool keyEquals(const char* key, const size_t len, const char* expected) {
  const size_t expectedLen = strlen(expected);
  return len == expectedLen && memcmp(key, expected, len) == 0;
}

int formatRank(const char* format) {
  if (strcmp(format, "EPUB3") == 0 || strcmp(format, "EPUB3FL") == 0) return 3;
  if (strcmp(format, "EPUB") == 0) return 2;
  if (strcmp(format, "KEPUB") == 0) return 1;
  return 0;
}

}  // namespace

KoboFeedParser::KoboFeedParser(EntryCallback cb, void* context)
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}),
      callback(cb),
      callbackContext(context) {}

void KoboFeedParser::reset() {
  parser.reset();
  depth = 0;
  entry = kobo::FeedEntry{};
  pendingUrl[0] = '\0';
  pendingFormat[0] = '\0';
  pendingSize = 0;
  itemId[0] = '\0';
  tagItemCount = 0;
  entries = 0;
  progressSet = false;
  done = false;
}

void KoboFeedParser::feed(const char* data, const size_t len) {
  if (done || parser.hasError()) return;
  parser.feed(data, len);
}

void KoboFeedParser::setKey(const char* key, const size_t len) {
  if (depth == 0) return;
  Frame& frame = frames[depth - 1];
  const size_t n = len < sizeof(frame.key) - 1 ? len : sizeof(frame.key) - 1;
  memcpy(frame.key, key, n);
  frame.key[n] = '\0';
}

bool KoboFeedParser::keyIs(const char* key) const { return depth > 0 && strcmp(frames[depth - 1].key, key) == 0; }

KoboFeedParser::Ctx KoboFeedParser::childContext(const Ctx parent, const char* key) const {
  if (parent == Ctx::Wrapper) {
    // Every calibre-web feed element is one named wrapper whose value is the
    // entry payload object. Unknown wrappers (e.g. DeletedTag) are skipped.
    if (strcmp(key, "NewEntitlement") == 0 || strcmp(key, "ChangedEntitlement") == 0 ||
        strcmp(key, "ChangedReadingState") == 0 || strcmp(key, "NewTag") == 0 || strcmp(key, "ChangedTag") == 0) {
      return Ctx::Entry;
    }
    return Ctx::Other;
  }
  if (parent == Ctx::Entry) {
    if (strcmp(key, "BookEntitlement") == 0) return Ctx::BookEntitlement;
    if (strcmp(key, "BookMetadata") == 0) return Ctx::BookMetadata;
    if (strcmp(key, "ReadingState") == 0) return Ctx::ReadingState;
    if (strcmp(key, "Tag") == 0) return Ctx::Tag;
    return Ctx::Other;
  }
  if (parent == Ctx::ReadingState) {
    if (strcmp(key, "CurrentBookmark") == 0) return Ctx::Bookmark;
    if (strcmp(key, "StatusInfo") == 0) return Ctx::StatusInfo;
    if (strcmp(key, "Statistics") == 0) return Ctx::Statistics;
    return Ctx::Other;
  }
  if (parent == Ctx::Bookmark) {
    return strcmp(key, "Location") == 0 ? Ctx::Location : Ctx::Other;
  }
  if (parent == Ctx::BookMetadata) {
    if (strcmp(key, "Series") == 0) return Ctx::Series;
    if (strcmp(key, "DownloadUrls") == 0) return Ctx::DownloadUrls;
    if (strcmp(key, "Contributors") == 0) return Ctx::Contributors;
    return Ctx::Other;
  }
  if (parent == Ctx::Tag && strcmp(key, "Items") == 0) return Ctx::TagItems;
  return Ctx::Other;
}

void KoboFeedParser::push(const Ctx ctx) {
  if (depth >= MAX_DEPTH) return;
  frames[depth].ctx = ctx;
  frames[depth].key[0] = '\0';
  depth++;
}

void KoboFeedParser::pop() {
  if (depth == 0) return;
  depth--;
}

void KoboFeedParser::beginEntry(const char* key, const size_t len) {
  entry = kobo::FeedEntry{};
  progressSet = false;
  pendingUrl[0] = '\0';
  pendingFormat[0] = '\0';
  pendingSize = 0;
  itemId[0] = '\0';
  tagItemCount = 0;
  if (keyEquals(key, len, "ChangedReadingState")) {
    entry.kind = kobo::EntryKind::ReadingState;
  } else if (keyEquals(key, len, "NewTag") || keyEquals(key, len, "ChangedTag")) {
    entry.kind = kobo::EntryKind::Tag;
  } else {
    entry.kind = kobo::EntryKind::Book;
    entry.isNew = keyEquals(key, len, "NewEntitlement");
  }
}

void KoboFeedParser::emitEntry() {
  if (entry.contentId[0] == '\0') return;
  if (entry.kind == kobo::EntryKind::Tag) return;  // tags emit one event per item
  entries++;
  if (callback) callback(callbackContext, entry);
}

void KoboFeedParser::emitTagItems() {
  for (uint8_t i = 0; i < tagItemCount; i++) {
    copyField(entry.contentId, sizeof(entry.contentId), tagItemIds[i], strlen(tagItemIds[i]));
    entries++;
    if (callback) callback(callbackContext, entry);
  }
  tagItemCount = 0;
  entry.contentId[0] = '\0';
}

void KoboFeedParser::appendAuthor(const char* value, const size_t len) {
  char normalized[kobo::AUTHOR_MAX];
  kobo::copyNormalizedField(normalized, sizeof(normalized), value, len);
  const size_t normalizedLen = strlen(normalized);

  const size_t used = strlen(entry.author);
  if (used >= kobo::AUTHOR_MAX - 1) return;
  size_t room = kobo::AUTHOR_MAX - 1 - used;
  if (used > 0) {
    if (room < 2) return;
    memcpy(entry.author + used, ", ", 2);
    room -= 2;
  }
  size_t n = normalizedLen < room ? normalizedLen : room;
  while (n > 0 && n < normalizedLen && (static_cast<unsigned char>(normalized[n]) & 0xC0) == 0x80) n--;
  memcpy(entry.author + used + (used > 0 ? 2 : 0), normalized, n);
  entry.author[used + (used > 0 ? 2 : 0) + n] = '\0';
}

void KoboFeedParser::sOnKey(void* ctx, const char* key, const size_t len) {
  static_cast<KoboFeedParser*>(ctx)->setKey(key, len);
}

void KoboFeedParser::sOnString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<KoboFeedParser*>(ctx);
  if (self->depth == 0) return;
  const Ctx context = self->frames[self->depth - 1].ctx;
  const char* key = self->frames[self->depth - 1].key;

  switch (context) {
    case Ctx::BookEntitlement:
      if (strcmp(key, "Id") == 0) copyField(self->entry.contentId, sizeof(self->entry.contentId), value, len);
      break;
    case Ctx::BookMetadata:
      if (strcmp(key, "Title") == 0) {
        kobo::copyNormalizedField(self->entry.title, sizeof(self->entry.title), value, len);
      }
      break;
    case Ctx::ReadingState:
      if (strcmp(key, "EntitlementId") == 0) {
        copyField(self->entry.contentId, sizeof(self->entry.contentId), value, len);
      } else if (strcmp(key, "LastModified") == 0) {
        self->entry.readingStateLastModified = kobo::parseIso8601Utc(value);
      }
      break;
    case Ctx::StatusInfo:
      if (strcmp(key, "Status") == 0) {
        if (strcmp(value, "Finished") == 0) {
          self->entry.readStatus = kobo::ReadStatus::Finished;
        } else if (strcmp(value, "Reading") == 0) {
          self->entry.readStatus = kobo::ReadStatus::Reading;
        } else {
          self->entry.readStatus = kobo::ReadStatus::Unread;
        }
      } else if (strcmp(key, "LastModified") == 0 && self->entry.readingStateLastModified == 0) {
        self->entry.readingStateLastModified = kobo::parseIso8601Utc(value);
      }
      break;
    case Ctx::Location:
      if (strcmp(key, "Value") == 0) {
        copyField(self->entry.locationValue, sizeof(self->entry.locationValue), value, len);
      } else if (strcmp(key, "Type") == 0) {
        copyField(self->entry.locationType, sizeof(self->entry.locationType), value, len);
      }
      break;
    case Ctx::Series:
      if (strcmp(key, "Name") == 0) {
        kobo::copyNormalizedField(self->entry.series, sizeof(self->entry.series), value, len);
      }
      break;
    case Ctx::DownloadUrl:
      if (strcmp(key, "Url") == 0) {
        copyField(self->pendingUrl, sizeof(self->pendingUrl), value, len);
      } else if (strcmp(key, "Format") == 0) {
        copyField(self->pendingFormat, sizeof(self->pendingFormat), value, len);
      }
      break;
    case Ctx::Contributors:
      self->appendAuthor(value, len);
      break;
    case Ctx::Tag:
      if (strcmp(key, "Id") == 0) {
        copyField(self->entry.tagId, sizeof(self->entry.tagId), value, len);
      } else if (strcmp(key, "Name") == 0) {
        kobo::copyNormalizedField(self->entry.tagName, sizeof(self->entry.tagName), value, len);
      }
      break;
    case Ctx::TagItem:
      if (strcmp(key, "RevisionId") == 0) copyField(self->itemId, sizeof(self->itemId), value, len);
      break;
    default:
      break;
  }
}

void KoboFeedParser::sOnNumber(void* ctx, const char* value, const size_t /*len*/) {
  auto* self = static_cast<KoboFeedParser*>(ctx);
  if (self->depth == 0) return;
  const Ctx context = self->frames[self->depth - 1].ctx;
  const char* key = self->frames[self->depth - 1].key;

  if (context == Ctx::Bookmark && (strcmp(key, "ProgressPercent") == 0 ||
                                   (strcmp(key, "ContentSourceProgressPercent") == 0 && !self->progressSet))) {
    const float percent = strtof(value, nullptr);
    const float clamped = percent < 0.0f ? 0.0f : (percent > 100.0f ? 100.0f : percent);
    self->entry.progressBp = static_cast<uint16_t>(clamped * 100.0f + 0.5f);
    self->progressSet = true;
  } else if (context == Ctx::DownloadUrl && strcmp(key, "Size") == 0) {
    self->pendingSize = static_cast<uint32_t>(strtoul(value, nullptr, 10));
  }
}

void KoboFeedParser::sOnBool(void* ctx, const bool value) {
  auto* self = static_cast<KoboFeedParser*>(ctx);
  if (self->depth == 0) return;
  if (self->frames[self->depth - 1].ctx == Ctx::BookEntitlement && self->keyIs("IsRemoved")) {
    self->entry.isRemoved = value;
  }
}

void KoboFeedParser::sOnNull(void* /*ctx*/) {}

void KoboFeedParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<KoboFeedParser*>(ctx);
  if (self->depth == 0) {
    self->push(Ctx::Other);
    return;
  }
  const Frame& parent = self->frames[self->depth - 1];
  if (parent.ctx == Ctx::TopArray) {
    self->push(Ctx::Wrapper);
    return;
  }
  if (parent.ctx == Ctx::Wrapper) {
    const Ctx child = self->childContext(Ctx::Wrapper, parent.key);
    if (child == Ctx::Entry) {
      char entryKey[40];
      snprintf(entryKey, sizeof(entryKey), "%s", parent.key);
      self->beginEntry(entryKey, strlen(entryKey));
    }
    self->push(child);
    return;
  }

  Ctx child = Ctx::Other;
  if (parent.ctx == Ctx::DownloadUrls) {
    child = Ctx::DownloadUrl;
  } else if (parent.ctx == Ctx::TagItems) {
    child = Ctx::TagItem;
  } else {
    child = self->childContext(parent.ctx, parent.key);
  }
  self->push(child);
}

void KoboFeedParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<KoboFeedParser*>(ctx);
  if (self->depth == 0) return;
  const Ctx ended = self->frames[self->depth - 1].ctx;

  if (ended == Ctx::DownloadUrl) {
    if (self->entry.downloadUrl[0] == '\0' ||
        formatRank(self->pendingFormat) > formatRank(self->entry.downloadFormat)) {
      memcpy(self->entry.downloadUrl, self->pendingUrl, sizeof(self->entry.downloadUrl));
      memcpy(self->entry.downloadFormat, self->pendingFormat, sizeof(self->entry.downloadFormat));
      self->entry.downloadSize = self->pendingSize;
    }
    self->pendingUrl[0] = '\0';
    self->pendingFormat[0] = '\0';
    self->pendingSize = 0;
  } else if (ended == Ctx::TagItem) {
    if (self->itemId[0] != '\0' && self->tagItemCount < MAX_TAG_ITEMS) {
      copyField(self->tagItemIds[self->tagItemCount], sizeof(self->tagItemIds[0]), self->itemId,
                strlen(self->itemId));
      self->tagItemCount++;
    }
    self->itemId[0] = '\0';
  } else if (ended == Ctx::Tag) {
    self->emitTagItems();
  } else if (ended == Ctx::Entry) {
    self->emitEntry();
  }
  self->pop();
}

void KoboFeedParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<KoboFeedParser*>(ctx);
  if (self->depth == 0) {
    self->push(Ctx::TopArray);
    return;
  }
  const Frame& parent = self->frames[self->depth - 1];
  Ctx child = Ctx::Other;
  if (parent.ctx == Ctx::BookMetadata && strcmp(parent.key, "DownloadUrls") == 0) {
    child = Ctx::DownloadUrls;
  } else if (parent.ctx == Ctx::BookMetadata && strcmp(parent.key, "Contributors") == 0) {
    child = Ctx::Contributors;
  } else if (parent.ctx == Ctx::Tag && strcmp(parent.key, "Items") == 0) {
    child = Ctx::TagItems;
  }
  self->push(child);
}

void KoboFeedParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<KoboFeedParser*>(ctx);
  if (self->depth == 0) return;
  const Ctx ended = self->frames[self->depth - 1].ctx;
  self->pop();
  if (ended == Ctx::TopArray) self->done = true;
}

#endif  // CROSSPOINT_KOBO_SYNC
