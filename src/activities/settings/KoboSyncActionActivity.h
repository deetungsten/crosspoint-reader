#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <string>

#include "activities/Activity.h"

// One-shot network action for Kobo sync from Home or the Kobo settings screen.
// It owns the Wi-Fi connect/teardown lifecycle so the settings list itself never
// blocks on the radio and runs the full incremental catalog sync.
class KoboSyncActionActivity final : public Activity {
 public:
  // returnToHome is set by the Home entry so the result screen returns to the
  // Kobo Sync row; settings launches leave it false and pop back to settings.
  KoboSyncActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool returnToHome = false)
      : Activity("KoboSyncAction", renderer, mappedInput), returnToHome(returnToHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }

 private:
  enum class State { RUNNING, SUCCESS, FAILED };

  State state = State::RUNNING;
  const bool returnToHome;
  std::string statusMessage;
  bool shouldTearDownWifiOnExit = false;

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void runAction();
  void finishAction();
};

#endif  // CROSSPOINT_KOBO_SYNC
