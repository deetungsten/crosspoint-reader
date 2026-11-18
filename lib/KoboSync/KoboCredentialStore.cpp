#include "KoboCredentialStore.h"

#include <KoboSyncConfig.h>

#if CROSSPOINT_KOBO_SYNC

#include <Logging.h>
#include <ObfuscationUtils.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

namespace {
constexpr uint8_t CONFIG_VERSION = 1;

void trimTrailingSlashes(std::string& value) {
  while (!value.empty() && value.back() == '/') value.pop_back();
}

std::string bounded(const std::string& value, const size_t maxLen) {
  if (value.size() <= maxLen) return value;
  return value.substr(0, maxLen);
}
}  // namespace

void KoboCredentialStore::generateDeviceId() {
  uint8_t bytes[16] = {};
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);  // variant 1
  char id[37];
  snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1],
           bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
           bytes[12], bytes[13], bytes[14], bytes[15]);
  deviceId = id;
}

void KoboCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["endpoint"] = endpoint;
  doc["token_obf"] = obfuscation::obfuscateToBase64(authToken);
  doc["deviceId"] = deviceId;
  doc["lastSyncToken"] = lastSyncToken;
  doc["cookie_obf"] = obfuscation::obfuscateToBase64(cookie);
  doc["lastSyncTime"] = lastSyncTime;
  doc["syncBehavior"] = static_cast<uint8_t>(syncBehavior);
  doc["syncOnOpen"] = syncOnOpen;
  doc["skipOpenSync"] = skipOpenSync;
  doc["pendingContentId"] = pendingContentId;
  doc["pendingProgressBp"] = pendingProgressBp;
  doc["pendingStatus"] = pendingStatus;
}

bool KoboCredentialStore::fromJson(JsonVariantConst doc) {
  endpoint = bounded(doc["endpoint"] | "", MAX_ENDPOINT);
  bool tokenOk = false;
  authToken = obfuscation::deobfuscateFromBase64(doc["token_obf"] | "", MAX_TOKEN, &tokenOk, nullptr);
  if (!tokenOk) authToken.clear();
  deviceId = bounded(doc["deviceId"] | "", 36);
  lastSyncToken = bounded(doc["lastSyncToken"] | "", MAX_SYNC_TOKEN);
  bool cookieOk = false;
  cookie = obfuscation::deobfuscateFromBase64(doc["cookie_obf"] | "", MAX_COOKIE, &cookieOk, nullptr);
  if (!cookieOk) cookie.clear();
  lastSyncTime = doc["lastSyncTime"] | static_cast<int64_t>(0);

  const uint8_t behavior = doc["syncBehavior"] | static_cast<uint8_t>(KoboSyncBehavior::SMART);
  syncBehavior = behavior == static_cast<uint8_t>(KoboSyncBehavior::ASK_EVERY_TIME) ? KoboSyncBehavior::ASK_EVERY_TIME
                                                                                    : KoboSyncBehavior::SMART;
  syncOnOpen = doc["syncOnOpen"] | false;
  skipOpenSync = doc["skipOpenSync"] | false;
  pendingContentId = bounded(doc["pendingContentId"] | "", 36);
  pendingProgressBp = doc["pendingProgressBp"] | static_cast<uint16_t>(0);
  pendingStatus = doc["pendingStatus"] | static_cast<uint8_t>(0);

  bool resave = false;
  if (deviceId.size() != 36) {
    generateDeviceId();
    resave = true;
  }
  if ((doc["cfgVersion"] | 0) != CONFIG_VERSION) resave = true;
  if (resave) requestResave();
  return true;
}

void KoboCredentialStore::ensureDeviceId() {
  if (deviceId.size() == 36) return;
  generateDeviceId();
  saveToFile();
}

void KoboCredentialStore::setEndpoint(const std::string& value) {
  std::string next = bounded(value, MAX_ENDPOINT);
  while (!next.empty() && (next.back() == ' ' || next.back() == '\t')) next.pop_back();
  while (!next.empty() && (next.front() == ' ' || next.front() == '\t')) next.erase(next.begin());
  // calibre-web's "Kobo Sync Key" page shows the full api_endpoint including
  // "/kobo/<token>". Accept that paste: split the token out when the field is
  // still empty and keep only the base URL, which is what the routes expect.
  const size_t kobo = next.find("/kobo/");
  if (kobo != std::string::npos) {
    std::string tail = next.substr(kobo + 6);
    trimTrailingSlashes(tail);
    const size_t slash = tail.find('/');
    if (slash != std::string::npos) tail.resize(slash);
    // A regenerated key is normally pasted as the full api_endpoint; let that
    // replace an older token instead of silently keeping the stale one.
    if (!tail.empty() && tail.size() < MAX_TOKEN && tail != authToken) {
      authToken = tail;
      clearSyncState();
    }
    next.resize(kobo);
  }
  while (!next.empty() && next.back() == '/') next.pop_back();
  if (next.size() > 5 && next.compare(next.size() - 5, 5, "/kobo") == 0) next.resize(next.size() - 5);
  if (next != endpoint) clearSyncState();
  endpoint = std::move(next);
}

std::string KoboCredentialStore::getBaseUrl() const {
  std::string url = endpoint;
  if (url.empty()) return url;
  if (url.find("://") == std::string::npos) url = "http://" + url;
  trimTrailingSlashes(url);
  return url;
}

void KoboCredentialStore::setAuthToken(const std::string& value) {
  std::string next = bounded(value, MAX_TOKEN);
  while (!next.empty() && (next.back() == ' ' || next.back() == '\t')) next.pop_back();
  while (!next.empty() && (next.front() == ' ' || next.front() == '\t')) next.erase(next.begin());
  if (next != authToken) clearSyncState();
  authToken = std::move(next);
}

void KoboCredentialStore::setLastSync(const std::string& token, const int64_t epochSeconds) {
  lastSyncToken = bounded(token, MAX_SYNC_TOKEN);
  lastSyncTime = epochSeconds;
}

void KoboCredentialStore::clearSyncState() {
  lastSyncToken.clear();
  cookie.clear();
  lastSyncTime = 0;
  clearPendingUpload();
}

bool KoboCredentialStore::consumeSkipOpenSync() {
  const bool suppressed = skipOpenSync;
  skipOpenSync = false;
  return suppressed;
}

void KoboCredentialStore::queueProgressUpload(const std::string& contentId, const uint16_t progressBp,
                                              const uint8_t status) {
  pendingContentId = bounded(contentId, 36);
  pendingProgressBp = progressBp;
  pendingStatus = status;
}

void KoboCredentialStore::clearPendingUpload() {
  pendingContentId.clear();
  pendingProgressBp = 0;
  pendingStatus = 0;
}

void KoboCredentialStore::clearPendingUploadFor(const std::string& contentId) {
  if (pendingContentId == contentId) clearPendingUpload();
}

void KoboCredentialStore::setCookie(const std::string& value) { cookie = bounded(value, MAX_COOKIE); }

void KoboCredentialStore::setSyncBehavior(const KoboSyncBehavior behavior) { syncBehavior = behavior; }

void KoboCredentialStore::clearCredentials() {
  endpoint.clear();
  authToken.clear();
  clearSyncState();
  saveToFile();
  LOG_DBG("KOBO", "Cleared Kobo credentials");
}

#endif  // CROSSPOINT_KOBO_SYNC
