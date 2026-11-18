#include "KoboSyncSettingsActivity.h"

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "KoboCatalog.h"
#include "KoboCredentialStore.h"
#include "KoboSyncActionActivity.h"
#include "MappedInputManager.h"
#include "activities/library/KoboShelfActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int IDX_SERVER_URL = 0;
constexpr int IDX_AUTH_TOKEN = 1;
constexpr int IDX_TEST_CONNECTION = 2;
constexpr int IDX_SYNC_NOW = 3;
constexpr int IDX_SYNC_BEHAVIOR = 4;
constexpr int IDX_SYNC_ON_OPEN = 5;
constexpr int IDX_BROWSE_SHELF = 6;
constexpr int IDX_CLEAR = 7;

constexpr StrId menuNames[KoboSyncSettingsActivity::MENU_ITEMS] = {
    StrId::STR_KOBO_SERVER_URL,   StrId::STR_KOBO_AUTH_TOKEN,       StrId::STR_KOBO_TEST_CONNECTION,
    StrId::STR_KOBO_SYNC_NOW,     StrId::STR_SYNC_BEHAVIOR,         StrId::STR_KOBO_SYNC_ON_OPEN,
    StrId::STR_KOBO_BROWSE_SHELF, StrId::STR_KOBO_CLEAR_CREDENTIALS};
}  // namespace

KoboSyncSettingsActivity::KoboSyncSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("KoboSyncSettings", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int KoboSyncSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* KoboSyncSettingsActivity::headerTitle() const { return tr(STR_KOBO_SYNC); }

bool KoboSyncSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void KoboSyncSettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  app.clearTapFlash();

  if (index == IDX_SERVER_URL) {
    const std::string current = KOBO_STORE.getEndpoint();
    const std::string prefill = current.empty() ? "http://" : current;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOBO_SERVER_URL),
                                                                   prefill, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) return;
                             const std::string& text = std::get<KeyboardResult>(result.data).text;
                             KOBO_STORE.setEndpoint(text == "http://" || text == "https://" ? "" : text);
                             KOBO_STORE.saveToFile();
                             // Rows and path links belong to the previous server/user identity.
                             KoboCatalog::removeAll();
                           });
  } else if (index == IDX_AUTH_TOKEN) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOBO_AUTH_TOKEN),
                                                                   KOBO_STORE.getAuthToken(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) return;
                             KOBO_STORE.setAuthToken(std::get<KeyboardResult>(result.data).text);
                             KOBO_STORE.saveToFile();
                             KoboCatalog::removeAll();
                           });
  } else if (index == IDX_TEST_CONNECTION) {
    if (!KOBO_STORE.hasCredentials()) return;
    startActivityForResult(
        std::make_unique<KoboSyncActionActivity>(renderer, mappedInput, KoboSyncActionActivity::Mode::TEST_CONNECTION),
        [](const ActivityResult&) {});
  } else if (index == IDX_SYNC_NOW) {
    if (!KOBO_STORE.hasCredentials()) return;
    startActivityForResult(
        std::make_unique<KoboSyncActionActivity>(renderer, mappedInput, KoboSyncActionActivity::Mode::SYNC_NOW),
        [](const ActivityResult&) {});
  } else if (index == IDX_SYNC_BEHAVIOR) {
    constexpr StrId behaviorNames[2] = {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC};
    optionPopup.show(
        StrId::STR_SYNC_BEHAVIOR, behaviorNames, 2, static_cast<int>(KOBO_STORE.getSyncBehavior()),
        [](const int selected) {
          KOBO_STORE.setSyncBehavior(selected == 0 ? KoboSyncBehavior::ASK_EVERY_TIME : KoboSyncBehavior::SMART);
          KOBO_STORE.saveToFile();
        });
    requestUpdate();
  } else if (index == IDX_SYNC_ON_OPEN) {
    KOBO_STORE.setSyncOnOpen(!KOBO_STORE.getSyncOnOpen());
    KOBO_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_BROWSE_SHELF) {
    if (!KOBO_STORE.hasCredentials()) return;
    startActivityForResult(std::make_unique<KoboShelfActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  } else if (index == IDX_CLEAR) {
    if (!KOBO_STORE.hasCredentials()) return;
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_KOBO_CLEAR_CREDENTIALS),
                                               tr(STR_KOBO_CLEAR_CONFIRM), tr(STR_CANCEL), tr(STR_CONFIRM)),
        [this](const ActivityResult& result) {
          if (result.isCancelled) return;
          KOBO_STORE.clearCredentials();
          KoboCatalog::removeAll();
          requestUpdate(true);
        });
  }
}

void KoboSyncSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const bool configured = KOBO_STORE.hasCredentials();
  for (int i = 0; i < MENU_ITEMS; i++) {
    if (i == IDX_SERVER_URL) {
      rowValues_[i] = KOBO_STORE.getEndpoint();
      if (rowValues_[i].empty()) rowValues_[i] = tr(STR_NOT_SET);
    } else if (i == IDX_AUTH_TOKEN) {
      rowValues_[i] = KOBO_STORE.getAuthToken().empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == IDX_SYNC_BEHAVIOR) {
      rowValues_[i] =
          KOBO_STORE.getSyncBehavior() == KoboSyncBehavior::SMART ? tr(STR_SMART_SYNC) : tr(STR_ASK_EVERY_TIME);
    } else if (i == IDX_TEST_CONNECTION || i == IDX_SYNC_NOW || i == IDX_BROWSE_SHELF || i == IDX_CLEAR) {
      rowValues_[i] = configured ? "" : std::string("[") + tr(STR_KOBO_NO_CREDENTIALS) + "]";
    } else {
      rowValues_[i].clear();
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }
  GUI.setCheckboxRow(rowItems_[IDX_SYNC_ON_OPEN], KOBO_STORE.getSyncOnOpen());

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void KoboSyncSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

#endif  // CROSSPOINT_KOBO_SYNC
