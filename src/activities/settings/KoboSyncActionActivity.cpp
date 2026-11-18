#include "KoboSyncActionActivity.h"

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "KoboCatalog.h"
#include "KoboCredentialStore.h"
#include "KoboSyncClient.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KoboSyncActionActivity::onEnter() {
  Activity::onEnter();
  state = State::RUNNING;
  statusMessage = mode == Mode::TEST_CONNECTION ? tr(STR_KOBO_TEST_CONNECTION) : tr(STR_KOBO_SYNCING);

  if (WiFi.status() == WL_CONNECTED) {
    requestUpdate();
    return;
  }

  shouldTearDownWifiOnExit = true;
  launchWifiSelection();
}

void KoboSyncActionActivity::onExit() {
  Activity::onExit();
  // A radio this activity brought up is released through the shared restart
  // path; reusing the stack-owned Wi-Fi driver is what keeps the next network
  // activity from starting with a fragmented heap.
  if (shouldTearDownWifiOnExit && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void KoboSyncActionActivity::launchWifiSelection() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KoboSyncActionActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    state = State::FAILED;
    statusMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate(true);
    return;
  }
  state = State::RUNNING;
  requestUpdate(true);
}

void KoboSyncActionActivity::runAction() {
  if (WiFi.status() != WL_CONNECTED) {
    state = State::FAILED;
    statusMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate(true);
    return;
  }

  KoboSyncClient::Error result = KoboSyncClient::NO_CREDENTIALS;
  if (mode == Mode::TEST_CONNECTION) {
    result = KoboSyncClient::testConnection();
  } else {
    KoboCatalog catalog;
    if (!catalog.open()) {
      state = State::FAILED;
      statusMessage = tr(STR_MEMORY_ERROR);
      requestUpdate(true);
      return;
    }
    KoboSyncClient::SyncStats stats;
    result = KoboSyncClient::syncCatalog(catalog, stats);
    catalog.close();
    if (result == KoboSyncClient::OK) {
      char message[64];
      snprintf(message, sizeof(message), tr(STR_KOBO_BOOKS_SYNCED), static_cast<unsigned>(stats.books));
      statusMessage = message;
    }
  }

  if (result == KoboSyncClient::OK) {
    state = State::SUCCESS;
    if (mode == Mode::TEST_CONNECTION) statusMessage = tr(STR_KOBO_CONNECTED);
  } else {
    state = State::FAILED;
    statusMessage = KoboSyncClient::errorString(result);
    LOG_ERR("KOBO", "Settings action failed: %s", statusMessage.c_str());
  }
  requestUpdate(true);
}

void KoboSyncActionActivity::loop() {
  if (state == State::RUNNING) {
    // Paint the status line before the blocking network call; without this the
    // screen would still show the settings list while TLS runs.
    requestUpdateAndWait();
    runAction();
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tapX, tapY)) {
    finish();
  }
}

void KoboSyncActionActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_KOBO_SYNC));

  const int midY = renderer.getScreenHeight() / 2;
  if (state == State::RUNNING) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY, statusMessage.c_str(), true, EpdFontFamily::BOLD);
  } else if (state == State::SUCCESS) {
    const char* heading = mode == Mode::TEST_CONNECTION ? tr(STR_KOBO_CONNECTED) : tr(STR_KOBO_SYNC_COMPLETE);
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 12, heading, true, EpdFontFamily::BOLD);
    if (mode == Mode::SYNC_NOW) {
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 18, statusMessage.c_str());
    }
  } else {
    const char* heading = mode == Mode::TEST_CONNECTION ? tr(STR_KOBO_CONNECTION_FAILED) : tr(STR_KOBO_SYNC_FAILED);
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 12, heading, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 18, statusMessage.c_str());
  }

  if (state != State::RUNNING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_KOBO_SYNC
