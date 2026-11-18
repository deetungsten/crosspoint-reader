#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <string>

#include "activities/Activity.h"

// One-shot network action for the Kobo settings screen. It owns the Wi-Fi
// connect/teardown lifecycle so the settings list itself never blocks on the
// radio: "Test Connection" registers the device, "Sync Now" runs the full
// incremental catalog sync.
class KoboSyncActionActivity final : public Activity {
 public:
  enum class Mode { TEST_CONNECTION, SYNC_NOW };

  KoboSyncActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode)
      : Activity("KoboSyncAction", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }

 private:
  enum class State { RUNNING, SUCCESS, FAILED };

  Mode mode;
  State state = State::RUNNING;
  std::string statusMessage;
  bool shouldTearDownWifiOnExit = false;

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void runAction();
};

#endif  // CROSSPOINT_KOBO_SYNC
