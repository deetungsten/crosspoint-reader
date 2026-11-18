#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

enum class KoboSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,
  SMART = 1,
};

// calibre-web Kobo credentials and resumable sync state. The auth token is the
// per-user path segment generated on the calibre-web profile page; the device
// id is a generated UUID that stays stable for this device.
class KoboCredentialStore : public PersistableStore<KoboCredentialStore> {
 private:
  std::string endpoint;
  std::string authToken;
  std::string deviceId;
  std::string lastSyncToken;
  std::string cookie;
  int64_t lastSyncTime = 0;
  KoboSyncBehavior syncBehavior = KoboSyncBehavior::SMART;
  bool syncOnOpen = false;
  bool skipOpenSync = false;
  // Deferred reading-position upload, written on reader close and flushed by
  // the next network action so leaving a book never blocks on TLS.
  std::string pendingContentId;
  uint16_t pendingProgressBp = 0;
  uint8_t pendingStatus = 0;

  KoboCredentialStore() = default;
  ~KoboCredentialStore() = default;
  friend class PersistableStore<KoboCredentialStore>;

  void generateDeviceId();

 public:
  static constexpr size_t MAX_ENDPOINT = 128;
  static constexpr size_t MAX_TOKEN = 64;
  static constexpr size_t MAX_SYNC_TOKEN = 2048;
  static constexpr size_t MAX_COOKIE = 768;

  static const char* getFilePath() { return "/.crosspoint/kobo.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool hasCredentials() const { return !endpoint.empty() && !authToken.empty(); }

  void setEndpoint(const std::string& value);
  const std::string& getEndpoint() const { return endpoint; }
  // Scheme-normalized endpoint with trailing slashes removed.
  std::string getBaseUrl() const;

  void setAuthToken(const std::string& value);
  const std::string& getAuthToken() const { return authToken; }

  const std::string& getDeviceId() const { return deviceId; }
  // Generates and persists a stable id on first boot (loadFromFile() cannot
  // call fromJson() when the file does not exist yet).
  void ensureDeviceId();

  void setLastSync(const std::string& token, int64_t epochSeconds);
  const std::string& getLastSyncToken() const { return lastSyncToken; }
  int64_t getLastSyncTime() const { return lastSyncTime; }
  void clearSyncState();

  void setCookie(const std::string& value);
  const std::string& getCookie() const { return cookie; }

  void setSyncBehavior(KoboSyncBehavior behavior);
  KoboSyncBehavior getSyncBehavior() const { return syncBehavior; }
  void setSyncOnOpen(bool enabled) { syncOnOpen = enabled; }
  bool getSyncOnOpen() const { return syncOnOpen; }

  // One-shot suppression of sync-on-open after a position sync returns to the
  // reader. Feature-owned so core state does not grow a Kobo field.
  void setSkipOpenSync(bool enabled) { skipOpenSync = enabled; }
  bool consumeSkipOpenSync();

  void queueProgressUpload(const std::string& contentId, uint16_t progressBp, uint8_t status);
  bool hasPendingUpload() const { return !pendingContentId.empty(); }
  const std::string& getPendingContentId() const { return pendingContentId; }
  uint16_t getPendingProgressBp() const { return pendingProgressBp; }
  uint8_t getPendingStatus() const { return pendingStatus; }
  void clearPendingUpload();
  // Clears the queued position only when it belongs to `contentId`; another
  // book's queued close position must survive this book's sync.
  void clearPendingUploadFor(const std::string& contentId);

  void clearCredentials();
};

#define KOBO_STORE KoboCredentialStore::getInstance()

#endif  // CROSSPOINT_KOBO_SYNC
