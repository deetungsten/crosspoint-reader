#include "KoboStatusSyncActivity.h"

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>

#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KoboCredentialStore.h"
#include "KoboSyncClient.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// 0.5% of the book: smaller differences are rounding noise from different
// layout engines, not a real conflict.
constexpr uint16_t CONFLICT_BASIS_POINTS = 50;

}  // namespace

KoboStatusSyncActivity::KoboStatusSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               std::string epubPath, std::string contentId, const float localProgress,
                                               const int localSpineIndex, const int localTotalPages)
    : Activity("KoboStatusSync", renderer, mappedInput),
      epubPath(std::move(epubPath)),
      contentId(std::move(contentId)),
      localProgress(std::clamp(localProgress, 0.0f, 1.0f)),
      localSpineIndex(localSpineIndex),
      localTotalPages(localTotalPages) {}

void KoboStatusSyncActivity::onEnter() {
  Activity::onEnter();

  if (!KOBO_STORE.hasCredentials() || contentId.empty()) {
    state = State::NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED) {
    state = State::FETCHING;
    requestUpdate();
    return;
  }
  launchWifiSelection();
}

void KoboStatusSyncActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    // Home/other exits also reboot into the reader; suppress the next
    // sync-on-open so that path cannot immediately re-enter sync.
    KOBO_STORE.setSkipOpenSync(true);
    KOBO_STORE.saveToFile();
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void KoboStatusSyncActivity::launchWifiSelection() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KoboStatusSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finishWith(State::FAILED, tr(STR_WIFI_CONN_FAILED));
    return;
  }
  state = State::FETCHING;
  requestUpdate(true);
}

void KoboStatusSyncActivity::performSync() {
  state = State::FETCHING;
  statusMessage = tr(STR_KOBO_FETCHING_STATE);
  requestUpdateAndWait();

  const auto result = KoboSyncClient::fetchReadingState(contentId, remote);
  if (result == KoboSyncClient::NOT_FOUND) {
    state = State::NO_REMOTE;
    actionPending = false;
    requestUpdate(true);
    return;
  }
  if (result != KoboSyncClient::OK) {
    finishWith(State::FAILED, KoboSyncClient::errorString(result));
    return;
  }

  const uint16_t localBasisPoints = static_cast<uint16_t>(localProgress * 10000.0f + 0.5f);
  if (KOBO_STORE.getSyncBehavior() == KoboSyncBehavior::SMART) {
    if (remote.progressBp > localBasisPoints + CONFLICT_BASIS_POINTS) {
      applyRemote();
    } else if (localBasisPoints > remote.progressBp + CONFLICT_BASIS_POINTS) {
      uploadLocal();
    } else {
      finishWith(State::DONE, tr(STR_ALREADY_SYNCED));
    }
    return;
  }

  decide();
}

void KoboStatusSyncActivity::decide() {
  char body[160];
  snprintf(body, sizeof(body), "%s %.1f%%\n%s %.1f%%", tr(STR_REMOTE_LABEL),
           static_cast<double>(remote.progressBp) / 100.0, tr(STR_LOCAL_LABEL),
           static_cast<double>(localProgress * 100.0f));
  const char* options[] = {tr(STR_APPLY_REMOTE), tr(STR_UPLOAD_LOCAL), tr(STR_CANCEL)};
  state = State::ASKING;
  actionPending = false;
  choicePopup.show(tr(STR_KOBO_STATUS_TITLE), body, options, 3, 0, [this](const int selected) {
    if (selected == 0) {
      applyRemote();
    } else if (selected == 1) {
      uploadLocal();
    } else {
      // The explicit Cancel returns to the reader. Back/tap-outside dismisses
      // the popup without a callback and is handled by loop().
      returnToReader();
    }
  });
  requestUpdate(true);
}

void KoboStatusSyncActivity::applyRemote() {
  state = State::APPLYING;
  statusMessage = tr(STR_APPLY_REMOTE);
  requestUpdateAndWait();

  auto epub = std::shared_ptr<Epub>(new (std::nothrow) Epub(epubPath, "/.crosspoint"));
  if (!epub) {
    finishWith(State::FAILED, tr(STR_MEMORY_ERROR));
    return;
  }
  epub->setupCacheDir();
  if (!epub->load(false, true)) {
    finishWith(State::FAILED, tr(STR_INDEX_FAILED));
    return;
  }

  SavedProgressPosition remoteProgress;
  remoteProgress.percentage = static_cast<float>(remote.progressBp) / 10000.0f;
  CrossPointPosition position;
  {
    // The mapper borrows the framebuffer; no render may run concurrently.
    RenderLock lock;
    GfxRenderer::FrameBufferLoan loan(renderer);
    position = ProgressMapper::toCrossPoint(epub, remoteProgress, renderer, localSpineIndex, localTotalPages);
  }

  std::optional<uint32_t> offset;
  if (position.hasVisibleTextOffset) offset = position.visibleTextOffset;
  if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, 0, offset)) {
    finishWith(State::FAILED, tr(STR_SAVE_PROGRESS_FAILED));
    return;
  }

  // The reader queued its pre-sync local position on the way out. Drop it only
  // for this book; flushing it later would overwrite the position just applied.
  KOBO_STORE.clearPendingUploadFor(contentId);
  KOBO_STORE.saveToFile();
  finishWith(State::DONE, tr(STR_APPLY_REMOTE));
}

void KoboStatusSyncActivity::uploadLocal() {
  state = State::UPLOADING;
  statusMessage = tr(STR_UPLOAD_LOCAL);
  requestUpdateAndWait();

  const uint16_t basisPoints = static_cast<uint16_t>(localProgress * 10000.0f + 0.5f);
  const kobo::ReadStatus status = basisPoints >= 9950 ? kobo::ReadStatus::Finished
                                  : basisPoints > 0   ? kobo::ReadStatus::Reading
                                                      : kobo::ReadStatus::Unread;
  const auto result = KoboSyncClient::putReadingState(contentId, localProgress * 100.0f, status, "");
  if (result != KoboSyncClient::OK) {
    finishWith(State::FAILED, KoboSyncClient::errorString(result));
    return;
  }
  KOBO_STORE.clearPendingUploadFor(contentId);
  KOBO_STORE.saveToFile();
  finishWith(State::DONE, tr(STR_UPLOAD_SUCCESS));
}

void KoboStatusSyncActivity::finishWith(const State next, const char* message) {
  state = next;
  actionPending = false;
  statusMessage = message != nullptr ? message : "";
  if (next == State::DONE) autoReturnAt = millis() + AUTO_RETURN_DELAY_MS;
  requestUpdate(true);
}

void KoboStatusSyncActivity::returnToReader() {
  // The reader is recreated after this activity, and it survives the Wi-Fi
  // heap-defrag reboot below. Persist the one-shot skip so sync-on-open cannot
  // start another fetch from the new reader instance.
  KOBO_STORE.setSkipOpenSync(true);
  KOBO_STORE.saveToFile();
  activityManager.goToReader(epubPath);
}

void KoboStatusSyncActivity::loop() {
  if (choicePopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (state == State::FETCHING || state == State::APPLYING || state == State::UPLOADING) {
    if (!actionPending) {
      actionPending = true;
      if (state == State::FETCHING) {
        performSync();
      } else if (state == State::APPLYING) {
        applyRemote();
      } else {
        uploadLocal();
      }
    }
    return;
  }

  if (state == State::NO_REMOTE) {
    if (!actionPending) {
      actionPending = true;
      if (KOBO_STORE.getSyncBehavior() == KoboSyncBehavior::SMART) {
        uploadLocal();
      } else {
        const char* options[] = {tr(STR_UPLOAD_LOCAL), tr(STR_CANCEL)};
        choicePopup.show(tr(STR_KOBO_STATUS_TITLE), tr(STR_NO_REMOTE_MSG), options, 2, 0, [this](const int selected) {
          if (selected == 0) {
            uploadLocal();
          } else {
            returnToReader();
          }
        });
        requestUpdate(true);
      }
    } else if (!choicePopup.isActive()) {
      // Back or a tap outside dismisses the popup without a callback.
      returnToReader();
    }
    return;
  }

  if (state == State::ASKING) {
    // The choice popup owns input; a dismissal without a selection is a cancel.
    if (!choicePopup.isActive()) returnToReader();
    return;
  }

  if (state == State::DONE && autoReturnAt != 0 && millis() >= autoReturnAt) {
    returnToReader();
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tapX, tapY)) {
    returnToReader();
  }
}

void KoboStatusSyncActivity::render(RenderLock&&) {
  if (choicePopup.processRender(renderer, mappedInput)) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_KOBO_STATUS_TITLE));

  const int midY = renderer.getScreenHeight() / 2;
  switch (state) {
    case State::FETCHING:
    case State::APPLYING:
    case State::UPLOADING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, statusMessage.c_str(), true, EpdFontFamily::BOLD);
      break;
    case State::DONE:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, statusMessage.c_str(), true, EpdFontFamily::BOLD);
      break;
    case State::NO_REMOTE:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
      break;
    case State::FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 12, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 18, statusMessage.c_str());
      break;
    case State::NO_CREDENTIALS:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 12, tr(STR_KOBO_NO_CREDENTIALS), true, EpdFontFamily::BOLD);
      break;
    case State::CONNECTING:
    case State::ASKING:
      break;
  }

  if (state != State::FETCHING && state != State::APPLYING && state != State::UPLOADING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_KOBO_SYNC
