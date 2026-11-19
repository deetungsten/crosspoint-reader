#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Kobo (calibre-web) sync settings: endpoint, per-user auth token, full sync,
// automatic downloads, and the reading-position behavior used by the reader.
class KoboSyncSettingsActivity final : public UiListActivity {
 public:
  static constexpr int MENU_ITEMS = 8;

  explicit KoboSyncSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void render(RenderLock&&) override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

  OptionPopup optionPopup;
  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};

#endif  // CROSSPOINT_KOBO_SYNC
