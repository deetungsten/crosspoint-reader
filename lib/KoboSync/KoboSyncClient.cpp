#include "KoboSyncClient.h"

#include <KoboSyncConfig.h>

#if CROSSPOINT_KOBO_SYNC

#include <ArduinoJson.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "KoboCredentialStore.h"
#include "KoboFeedParser.h"
#include "KoboText.h"
#include "KoboSyncTime.h"
#include "KoboSyncToken.h"

int KoboSyncClient::lastHttpCode = 0;

namespace {

constexpr int HTTP_TIMEOUT_MS = 30000;
constexpr uint16_t MAX_SYNC_PAGES = 20;
constexpr size_t MAX_METADATA_BYTES = 8192;
constexpr size_t MAX_STATE_BYTES = 4096;

// Same preflight floor as the KOReader client: a TLS handshake that cannot
// find this much contiguous room fails mid-record on the C3.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

struct ResponseInfo {
  int status = 0;
  std::string syncToken;
  bool continueSync = false;
};

bool insufficientHeap() {
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < MIN_FREE_FOR_TLS || heap.largestBlockBytes < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("KOBO", "Low heap for sync: %zu free (need %u), %zu max block (need %u)", heap.freeBytes, MIN_FREE_FOR_TLS,
            heap.largestBlockBytes, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

void copyField(char* dst, const size_t cap, const char* src, const size_t len) {
  if (cap == 0) return;
  size_t n = len < cap - 1 ? len : cap - 1;
  // src has only `len` valid bytes, so inspect src[n] only when truncated.
  if (n < len) {
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

std::string apiUrl(const char* suffix) {
  return KOBO_STORE.getBaseUrl() + "/kobo/" + KOBO_STORE.getAuthToken() + suffix;
}

void applyDeviceHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/json");
  http.addHeader("x-kobo-device-id", KOBO_STORE.getDeviceId());
  http.addHeader("x-kobo-device-model", "CrossPoint Reader");
  http.addHeader("x-kobo-product-name", "CrossPoint");
  http.addHeader("x-kobo-platform-id", "crosspoint-esp32");
#ifdef CROSSPOINT_VERSION
  http.addHeader("x-kobo-appversion", CROSSPOINT_VERSION);
#endif
  if (!KOBO_STORE.getCookie().empty()) http.addHeader("Cookie", KOBO_STORE.getCookie());
}

void mergeCookie(std::string& jar, const std::string& setCookie) {
  const size_t semicolon = setCookie.find(';');
  std::string pair = setCookie.substr(0, semicolon);
  while (!pair.empty() && (pair.back() == ' ' || pair.back() == '\t')) pair.pop_back();
  while (!pair.empty() && (pair.front() == ' ' || pair.front() == '\t')) pair.erase(pair.begin());
  const size_t equals = pair.find('=');
  if (equals == std::string::npos || equals == 0) return;
  const std::string name = pair.substr(0, equals);
  if (name == "deleted" || pair.find("=deleted") != std::string::npos) return;

  std::string next;
  size_t start = 0;
  int kept = 0;
  while (start < jar.size()) {
    const size_t end = jar.find("; ", start);
    const std::string part = jar.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t partEquals = part.find('=');
    if (partEquals != std::string::npos && part.substr(0, partEquals) != name && kept < 4) {
      if (!next.empty()) next += "; ";
      next += part;
      kept++;
    }
    if (end == std::string::npos) break;
    start = end + 2;
  }
  if (!next.empty()) next += "; ";
  next += pair;
  if (next.size() <= KoboCredentialStore::MAX_COOKIE) {
    jar = std::move(next);
  } else {
    jar = pair;
  }
}

void captureCookies(const freeink::SecureHttpClient& http) {
  // calibre-web sets one session cookie. Reading it directly avoids copying
  // the whole (up to 8 KB) response-header vector while TLS is still alive.
  const std::string setCookie = http.getHeader("set-cookie");
  if (setCookie.empty()) return;
  std::string jar = KOBO_STORE.getCookie();
  mergeCookie(jar, setCookie);
  KOBO_STORE.setCookie(jar);
}

bool headerIsContinue(const std::string& value) {
  if (value.size() != 8) return false;
  char lower[9];
  for (size_t i = 0; i < 8; i++) lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(value[i])));
  lower[8] = '\0';
  return strcmp(lower, "continue") == 0;
}

KoboSyncClient::Error request(const char* method, const std::string& path, const std::string& body,
                              const std::string& syncToken, const freeink::SecureHttpClient::DataCallback& onData,
                              ResponseInfo& info, KoboSyncClient::CancelPredicate cancelPredicate = nullptr,
                              void* cancelContext = nullptr) {
  info = ResponseInfo{};
  KoboSyncClient::lastHttpCode = 0;
  if (!KOBO_STORE.hasCredentials()) return KoboSyncClient::NO_CREDENTIALS;
  if (insufficientHeap()) return KoboSyncClient::LOW_MEMORY;

  const std::string url = apiUrl(path.c_str());
  LOG_DBG("KOBO", "%s %s", method, url.c_str());
  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
#ifdef CROSSPOINT_VERSION
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
#else
  http.setUserAgent("CrossPoint-ESP32");
#endif
  if (!http.begin(url)) {
    LOG_ERR("KOBO", "Bad URL: %s", url.c_str());
    return KoboSyncClient::NETWORK_ERROR;
  }
  applyDeviceHeaders(http);
  if (!syncToken.empty()) http.addHeader("x-kobo-synctoken", syncToken);
  if (!body.empty()) http.addHeader("Content-Type", "application/json");

  const auto abort = [cancelPredicate, cancelContext] {
    return cancelPredicate != nullptr && cancelPredicate(cancelContext);
  };
  int status = 0;
  if (strcmp(method, "GET") == 0) {
    status = http.GET(onData, abort);
  } else {
    status = http.sendRequest(method, reinterpret_cast<const uint8_t*>(body.data()), body.size(), onData, abort);
  }
  info.status = status;
  info.syncToken = http.getHeader("x-kobo-synctoken");
  info.continueSync = headerIsContinue(http.getHeader("x-kobo-sync"));
  captureCookies(http);
  KoboSyncClient::lastHttpCode = status;

  if (http.aborted()) return KoboSyncClient::CANCELLED;
  if (status <= 0) return KoboSyncClient::NETWORK_ERROR;
  if (status == 401 || status == 403) return KoboSyncClient::AUTH_FAILED;
  if (status == 404) return KoboSyncClient::NOT_FOUND;
  if (status < 200 || status >= 300) return KoboSyncClient::SERVER_ERROR;
  return KoboSyncClient::OK;
}

struct SyncContext {
  KoboCatalog* catalog = nullptr;
  KoboSyncClient::SyncStats* stats = nullptr;
  // A token-less sync starts from an empty catalog, so every NewEntitlement
  // can take the append fast path. When the token was discarded but rows
  // remain, treat the feed as incremental to avoid duplicate records.
  bool catalogWasEmpty = false;
};

void onEntry(void* ctx, const kobo::FeedEntry& entry) {
  auto* context = static_cast<SyncContext*>(ctx);
  if (context == nullptr || context->catalog == nullptr) return;

  if (entry.kind == kobo::EntryKind::Tag) {
    context->catalog->appendShelf(entry.contentId, entry.tagName);
    if (context->stats) context->stats->tags++;
    return;
  }

  KoboBookRecord record;
  // A NewEntitlement is new to this catalog; appending avoids a map search and
  // keeps the first sync from doing an O(N^2) sorted-map shift.
  const bool isNewBook = entry.kind == kobo::EntryKind::Book && entry.isNew && context->catalogWasEmpty;
  const bool exists = !isNewBook && context->catalog->findByContentId(entry.contentId, record);
  if (!exists) {
    record = KoboBookRecord{};
    copyField(record.contentId, sizeof(record.contentId), entry.contentId, strlen(entry.contentId));
  }

  if (entry.kind == kobo::EntryKind::Book) {
    if (entry.title[0] != '\0') copyField(record.title, sizeof(record.title), entry.title, strlen(entry.title));
    if (entry.author[0] != '\0') copyField(record.author, sizeof(record.author), entry.author, strlen(entry.author));
    if (entry.series[0] != '\0') copyField(record.series, sizeof(record.series), entry.series, strlen(entry.series));
    if (entry.isNew) record.flags |= kobo_flags::NEW_SINCE_SYNC;
    if (entry.isRemoved) {
      record.flags |= kobo_flags::REMOVED;
    } else {
      record.flags &= static_cast<uint8_t>(~kobo_flags::REMOVED);
    }
  }

  if (entry.readingStateLastModified != 0 || entry.progressBp != 0 || entry.readStatus != kobo::ReadStatus::Unread) {
    record.progressBp = entry.progressBp;
    record.readStatus = static_cast<uint8_t>(entry.readStatus);
    if (entry.readingStateLastModified > record.lastModified) record.lastModified = entry.readingStateLastModified;
  }

  const bool stored = isNewBook ? context->catalog->append(record) : context->catalog->upsert(record);
  if (!stored) {
    LOG_ERR("KOBO", "Failed to store book %s", entry.contentId);
  }
  if (context->stats) {
    if (entry.kind == kobo::EntryKind::Book) {
      context->stats->books++;
    } else {
      context->stats->readingStates++;
    }
  }
}

KoboSyncClient::Error collectBody(const std::string& path, std::string& body, const size_t cap,
                                  KoboSyncClient::CancelPredicate cancelPredicate = nullptr,
                                  void* cancelContext = nullptr) {
  body.clear();
  bool tooLarge = false;
  ResponseInfo info;
  const auto error = request(
      "GET", path, "", "",
      [&body, cap, &tooLarge](const uint8_t* data, const size_t len) {
        if (body.size() + len > cap) {
          tooLarge = true;
          return false;
        }
        body.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      info, cancelPredicate, cancelContext);
  if (error != KoboSyncClient::OK) return error;
  if (tooLarge) return KoboSyncClient::JSON_ERROR;
  return KoboSyncClient::OK;
}

}  // namespace

KoboSyncClient::Error KoboSyncClient::syncCatalog(KoboCatalog& catalog, SyncStats& stats,
                                                  const CancelPredicate cancelPredicate, void* cancelContext) {
  lastHttpCode = 0;
  stats = SyncStats{};
  if (!KOBO_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (!catalog.isOpen()) return SERVER_ERROR;

  // A recreated catalog (format bump or corruption) has no rows, so the old
  // incremental token must be dropped or the server would never resend them.
  if (catalog.wasReset()) {
    LOG_DBG("KOBO", "Catalog reset; discarding sync token");
    KOBO_STORE.clearSyncState();
    catalog.clearResetFlag();
  }

  // The feed's "NewEntitlement" flag is relative to the token; clear the old
  // markers before applying the new page set. A failed sync re-fetches the
  // same pages next time, so the markers return.
  catalog.clearNewFlags();

  std::string token = KOBO_STORE.getLastSyncToken();
  if (!token.empty() && !KoboSyncToken::fromHeader(token).valid()) {
    LOG_DBG("KOBO", "Discarding malformed sync token");
    token.clear();
  }

  SyncContext context{&catalog, &stats, catalog.count() == 0};
  auto parser = makeUniqueNoThrow<KoboFeedParser>(onEntry, &context);
  if (!parser) {
    LOG_ERR("KOBO", "OOM: feed parser");
    return LOW_MEMORY;
  }

  Error result = OK;
  for (uint16_t page = 0; page < MAX_SYNC_PAGES; page++) {
    parser->reset();
    ResponseInfo info;
    const auto feedSink = [&parser, cancelPredicate, cancelContext](const uint8_t* data, const size_t len) {
      if (cancelPredicate != nullptr && cancelPredicate(cancelContext)) return false;
      parser->feed(reinterpret_cast<const char*>(data), len);
      return !parser->hasError();
    };
    result = request("GET", "/v1/library/sync", "", token, feedSink, info, cancelPredicate, cancelContext);
    if (parser->hasError() || (result == OK && !parser->finished())) {
      result = JSON_ERROR;
      break;
    }
    if (result != OK) break;
    stats.pages++;
    if (info.syncToken.empty() || info.syncToken == token) break;
    token = info.syncToken;
    if (!info.continueSync) break;
  }

  catalog.finishSync();
  if (result == OK) {
    KOBO_STORE.setLastSync(token, trustedtime::trustedNow());
  }
  // Persist the cookie even after a failed page: it avoids a fresh login on
  // the retry, while the token stays at the last fully-applied page set.
  KOBO_STORE.saveToFile();
  if (result == OK) {
    // A position queued when the reader closed is flushed here, so "Sync Now"
    // and visiting the shelf upload the last book's progress without making
    // the reader exit path block on TLS.
    const auto pending = flushPendingProgress(cancelPredicate, cancelContext);
    if (pending != OK) LOG_ERR("KOBO", "Pending position upload failed: %d", static_cast<int>(pending));
  }
  LOG_DBG("KOBO", "Sync %s: books=%u states=%u tags=%u pages=%u token=%s", result == OK ? "complete" : "failed",
          stats.books, stats.readingStates, stats.tags, stats.pages, token.c_str());
  return result;
}

KoboSyncClient::Error KoboSyncClient::fetchMetadata(const std::string& contentId, kobo::Metadata& out,
                                                    const CancelPredicate cancelPredicate, void* cancelContext) {
  out = kobo::Metadata{};
  if (!KOBO_STORE.hasCredentials()) return NO_CREDENTIALS;
  const std::string path = "/v1/library/" + contentId + "/metadata";
  std::string body;
  const auto result = collectBody(path, body, MAX_METADATA_BYTES, cancelPredicate, cancelContext);
  if (result != OK) return result;

  JsonDocument filter;
  filter[0]["Title"] = true;
  filter[0]["Contributors"] = true;
  // Keep every download URL so the EPUB3/EPUB/KEPUB preference can pick the
  // best available format rather than whichever the server listed first.
  filter[0]["DownloadUrls"] = true;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("KOBO", "Metadata JSON: %s", error.c_str());
    return JSON_ERROR;
  }

  JsonObjectConst metadata = doc[0].as<JsonObjectConst>();
  if (metadata.isNull()) return JSON_ERROR;
  copyField(out.contentId, sizeof(out.contentId), contentId.c_str(), contentId.size());
  const char* title = metadata["Title"] | "";
  kobo::copyNormalizedField(out.title, sizeof(out.title), title, strlen(title));
  JsonArrayConst contributors = metadata["Contributors"].as<JsonArrayConst>();
  if (!contributors.isNull() && contributors.size() > 0) {
    const char* author = contributors[0] | "";
    kobo::copyNormalizedField(out.author, sizeof(out.author), author, strlen(author));
  }

  int bestRank = 0;
  for (JsonObjectConst url : metadata["DownloadUrls"].as<JsonArrayConst>()) {
    const char* format = url["Format"] | "";
    int rank = 0;
    if (strcmp(format, "EPUB3") == 0 || strcmp(format, "EPUB3FL") == 0) {
      rank = 3;
    } else if (strcmp(format, "EPUB") == 0) {
      rank = 2;
    } else if (strcmp(format, "KEPUB") == 0) {
      rank = 1;
    }
    if (rank <= bestRank) continue;
    const char* link = url["Url"] | "";
    if (link[0] == '\0') continue;
    bestRank = rank;
    copyField(out.downloadUrl, sizeof(out.downloadUrl), link, strlen(link));
    copyField(out.downloadFormat, sizeof(out.downloadFormat), format, strlen(format));
    out.downloadSize = url["Size"] | 0;
  }
  return out.downloadUrl[0] != '\0' ? OK : NOT_FOUND;
}

KoboSyncClient::Error KoboSyncClient::fetchReadingState(const std::string& contentId, kobo::ReadingState& out) {
  out = kobo::ReadingState{};
  if (!KOBO_STORE.hasCredentials()) return NO_CREDENTIALS;
  const std::string path = "/v1/library/" + contentId + "/state";
  std::string body;
  const auto result = collectBody(path, body, MAX_STATE_BYTES);
  if (result == NOT_FOUND) return NOT_FOUND;
  if (result != OK) return result;

  JsonDocument filter;
  filter[0]["CurrentBookmark"]["ProgressPercent"] = true;
  filter[0]["CurrentBookmark"]["ContentSourceProgressPercent"] = true;
  filter[0]["CurrentBookmark"]["Location"]["Value"] = true;
  filter[0]["CurrentBookmark"]["Location"]["Type"] = true;
  filter[0]["StatusInfo"]["Status"] = true;
  filter[0]["LastModified"] = true;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("KOBO", "State JSON: %s", error.c_str());
    return JSON_ERROR;
  }
  JsonObjectConst state = doc[0].as<JsonObjectConst>();
  if (state.isNull()) return NOT_FOUND;
  const float percent = state["CurrentBookmark"]["ProgressPercent"] | 0.0f;
  const float clamped = percent < 0.0f ? 0.0f : (percent > 100.0f ? 100.0f : percent);
  out.progressBp = static_cast<uint16_t>(clamped * 100.0f + 0.5f);
  const char* status = state["StatusInfo"]["Status"] | "";
  if (strcmp(status, "Finished") == 0) {
    out.readStatus = kobo::ReadStatus::Finished;
  } else if (strcmp(status, "Reading") == 0) {
    out.readStatus = kobo::ReadStatus::Reading;
  } else {
    out.readStatus = kobo::ReadStatus::Unread;
  }
  const char* location = state["CurrentBookmark"]["Location"]["Value"] | "";
  const char* locationType = state["CurrentBookmark"]["Location"]["Type"] | "";
  copyField(out.locationValue, sizeof(out.locationValue), location, strlen(location));
  copyField(out.locationType, sizeof(out.locationType), locationType, strlen(locationType));
  const char* modified = state["LastModified"] | "";
  out.lastModified = kobo::parseIso8601Utc(modified);
  out.found = true;
  return OK;
}

KoboSyncClient::Error KoboSyncClient::putReadingState(const std::string& contentId, const float progressPercent,
                                                      const kobo::ReadStatus status, const std::string& location,
                                                      const CancelPredicate cancelPredicate, void* cancelContext) {
  if (!KOBO_STORE.hasCredentials()) return NO_CREDENTIALS;
  const float clamped = progressPercent < 0.0f ? 0.0f : (progressPercent > 100.0f ? 100.0f : progressPercent);
  JsonDocument doc;
  JsonObject state = doc["ReadingStates"][0].to<JsonObject>();
  state["EntitlementId"] = contentId;
  JsonObject bookmark = state["CurrentBookmark"].to<JsonObject>();
  bookmark["ProgressPercent"] = clamped;
  bookmark["ContentSourceProgressPercent"] = clamped;
  // calibre-web indexes Location and Statistics unconditionally, so both keys
  // must exist. Empty objects are falsy there, which skips the optional
  // location/statistics updates instead of raising KeyError (HTTP 400).
  JsonObject bookmarkLocation = bookmark["Location"].to<JsonObject>();
  if (!location.empty()) {
    bookmarkLocation["Value"] = location;
    bookmarkLocation["Type"] = "KoboToc";
    bookmarkLocation["Source"] = "CrossPoint";
  }
  state["Statistics"].to<JsonObject>();  // keep the user's stored statistics
  state["StatusInfo"]["Status"] = status == kobo::ReadStatus::Finished  ? "Finished"
                                  : status == kobo::ReadStatus::Reading ? "Reading"
                                                                        : "ReadyToRead";
  std::string body;
  serializeJson(doc, body);

  std::string response;
  ResponseInfo info;
  return request(
      "PUT", "/v1/library/" + contentId + "/state", body, "",
      [&response](const uint8_t* data, const size_t len) {
        if (response.size() + len > MAX_STATE_BYTES) return false;
        response.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      info, cancelPredicate, cancelContext);
}

KoboSyncClient::Error KoboSyncClient::flushPendingProgress(const CancelPredicate cancelPredicate,
                                                           void* cancelContext) {
  if (!KOBO_STORE.hasPendingUpload()) return OK;
  const auto result = putReadingState(KOBO_STORE.getPendingContentId(),
                                      static_cast<float>(KOBO_STORE.getPendingProgressBp()) / 100.0f,
                                      static_cast<kobo::ReadStatus>(KOBO_STORE.getPendingStatus()), "", cancelPredicate,
                                      cancelContext);
  if (result == OK) {
    KOBO_STORE.clearPendingUpload();
    KOBO_STORE.saveToFile();
  }
  return result;
}

std::string KoboSyncClient::coverUrl(const std::string& contentId, const int width, const int height) {
  char suffix[128];
  snprintf(suffix, sizeof(suffix), "/%s/%d/%d/false/image.jpg", contentId.c_str(), width, height);
  return apiUrl(suffix);
}

const char* KoboSyncClient::errorString(const Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return tr(STR_KOBO_NO_CREDENTIALS);
    case NETWORK_ERROR:
      return tr(STR_KOBO_ERR_NETWORK);
    case AUTH_FAILED:
      return tr(STR_KOBO_ERR_AUTH);
    case SERVER_ERROR:
      return tr(STR_KOBO_ERR_SERVER);
    case JSON_ERROR:
      return tr(STR_KOBO_ERR_JSON);
    case NOT_FOUND:
      return tr(STR_KOBO_ERR_NOT_FOUND);
    case LOW_MEMORY:
      return tr(STR_KOBO_ERR_MEMORY);
    case CANCELLED:
      return tr(STR_KOBO_ERR_CANCELLED);
    default:
      return tr(STR_KOBO_ERR_SERVER);
  }
}

#endif  // CROSSPOINT_KOBO_SYNC
