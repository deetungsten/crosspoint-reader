#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <string>

#include "KoboSyncTypes.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Reading-position ("ReadingPosition") sync for one calibre-web Kobo book.
//
// The reader passes the local whole-book progress before releasing its Epub.
// The remote position is fetched first; SMART applies the further position
// automatically, ASK shows the same Apply/Upload choice as the KOReader flow.
// Applying a remote position lazily reloads the Epub only after the network
// call, so TLS never competes with the parser and layout cache.
class KoboStatusSyncActivity final : public Activity {
 public:
  KoboStatusSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                         std::string contentId, float localProgress, int localSpineIndex, int localTotalPages);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override {
    return state == State::FETCHING || state == State::APPLYING || state == State::UPLOADING;
  }

 private:
  enum class State {
    CONNECTING,
    FETCHING,
    ASKING,
    APPLYING,
    UPLOADING,
    DONE,
    NO_REMOTE,
    FAILED,
    NO_CREDENTIALS,
  };

  std::string epubPath;
  std::string contentId;
  float localProgress = 0.0f;
  int localSpineIndex = 0;
  int localTotalPages = 0;

  State state = State::CONNECTING;
  std::string statusMessage;
  kobo::ReadingState remote;
  OptionPopup choicePopup;
  bool actionPending = false;
  bool wifiActivated = false;
  unsigned long autoReturnAt = 0;

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void performSync();
  void decide();
  void applyRemote();
  void uploadLocal();
  void finishWith(State next, const char* message);
  void returnToReader();

  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1400;
};

#endif  // CROSSPOINT_KOBO_SYNC
