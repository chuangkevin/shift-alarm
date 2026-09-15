#pragma once
#include <stdint.h>
#include <stdio.h>

namespace deviceui {

constexpr uint32_t INACTIVITY_MS = 15000;
constexpr uint8_t MENU_ITEM_COUNT = 6;
constexpr uint8_t DETAIL_PAGE_COUNT = 5;
constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_MAX = SCREEN_SIZE - 1;
constexpr int QR_VERSION_8_MODULES = 49;
constexpr int QR_QUIET_MODULES = 4;
constexpr int QR_SCALE = 3;
constexpr int QR_QUIET_SIZE = (QR_VERSION_8_MODULES + 2 * QR_QUIET_MODULES) * QR_SCALE;
constexpr int QR_QUIET_X = (SCREEN_SIZE - QR_QUIET_SIZE) / 2;
constexpr int QR_QUIET_Y = 34;
constexpr int QR_X = QR_QUIET_X + QR_QUIET_MODULES * QR_SCALE;
constexpr int QR_Y = QR_QUIET_Y + QR_QUIET_MODULES * QR_SCALE;
constexpr int TITLE_Y = 4;
constexpr int TITLE_BOTTOM = 31;
constexpr int BOTTOM_TEXT_X = 78;
constexpr int BOTTOM_TEXT_Y = 214;

constexpr int qrModuleLeft() { return QR_X; }
constexpr int qrModuleTop() { return QR_Y; }
constexpr int qrModuleRight() { return QR_X + QR_VERSION_8_MODULES * QR_SCALE - 1; }
constexpr int qrModuleBottom() { return QR_Y + QR_VERSION_8_MODULES * QR_SCALE - 1; }
constexpr int qrQuietLeft() { return QR_QUIET_X; }
constexpr int qrQuietTop() { return QR_QUIET_Y; }
constexpr int qrQuietRight() { return QR_QUIET_X + QR_QUIET_SIZE - 1; }
constexpr int qrQuietBottom() { return QR_QUIET_Y + QR_QUIET_SIZE - 1; }

static_assert(QR_QUIET_SIZE == 171, "version 8 QR must occupy 171 pixels at scale 3");
static_assert(qrQuietLeft() >= 0 && qrQuietTop() >= 0 &&
                  qrQuietRight() <= SCREEN_MAX && qrQuietBottom() <= SCREEN_MAX,
              "QR quiet zone must fit the logical display");
static_assert(TITLE_BOTTOM < qrQuietTop(), "title must not overlap the QR quiet zone");
static_assert(BOTTOM_TEXT_Y > qrQuietBottom() && BOTTOM_TEXT_Y <= SCREEN_MAX,
              "bottom text must begin below the QR quiet zone");

enum class Page : uint8_t {
  Main,
  Menu,
  PhoneSetup,
  Connectivity,
  Schedule,
  Device,
  Update,
};

struct State {
  Page page = Page::Main;
  uint8_t selection = 0;
  uint32_t last_interaction_ms = 0;
};

inline Page detailPage(uint8_t selection) {
  return selection < DETAIL_PAGE_COUNT
             ? static_cast<Page>(static_cast<uint8_t>(Page::PhoneSetup) + selection)
             : Page::Main;
}

inline void left(State &state, uint32_t now) {
  state.last_interaction_ms = now;
  if (state.page == Page::Main) {
    state.page = Page::Menu;
    state.selection = (state.selection + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
  } else if (state.page == Page::Menu) {
    state.selection = (state.selection + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
  } else {
    uint8_t detail = static_cast<uint8_t>(state.page) - static_cast<uint8_t>(Page::PhoneSetup);
    state.page = detailPage((detail + DETAIL_PAGE_COUNT - 1) % DETAIL_PAGE_COUNT);
    state.selection = static_cast<uint8_t>(state.page) - static_cast<uint8_t>(Page::PhoneSetup);
  }
}

inline void right(State &state, uint32_t now) {
  state.last_interaction_ms = now;
  if (state.page == Page::Main) {
    state.page = Page::Menu;
    state.selection = (state.selection + 1) % MENU_ITEM_COUNT;
  } else if (state.page == Page::Menu) {
    state.selection = (state.selection + 1) % MENU_ITEM_COUNT;
  } else {
    uint8_t detail = static_cast<uint8_t>(state.page) - static_cast<uint8_t>(Page::PhoneSetup);
    state.page = detailPage((detail + 1) % DETAIL_PAGE_COUNT);
    state.selection = static_cast<uint8_t>(state.page) - static_cast<uint8_t>(Page::PhoneSetup);
  }
}

inline void center(State &state, uint32_t now) {
  state.last_interaction_ms = now;
  if (state.page == Page::Main || state.page == Page::Menu) state.page = detailPage(state.selection);
  else if (state.page != Page::Main) state.page = Page::Menu;
}

inline bool returnIfInactive(State &state, uint32_t now) {
  if (state.page == Page::Main || uint32_t(now - state.last_interaction_ms) < INACTIVITY_MS)
    return false;
  state.page = Page::Main;
  return true;
}

inline int64_t localDay(int64_t epoch) { return (epoch + 8 * 3600) / 86400; }

inline void countdown(int64_t now, int64_t next, char *out, size_t size) {
  if (next <= now) {
    snprintf(out, size, "尚無下一次鬧鐘");
    return;
  }
  const int64_t seconds = next - now;
  const int64_t minutes = (seconds + 59) / 60;
  const int64_t hours = minutes / 60;
  const int64_t day_delta = localDay(next) - localDay(now);
  if (day_delta == 0) {
    if (hours) snprintf(out, size, "今天 還有 %lld 小時 %lld 分鐘", (long long)hours,
                        (long long)(minutes % 60));
    else snprintf(out, size, "今天 還有 %lld 分鐘", (long long)minutes);
  } else if (seconds < 86400) {
    snprintf(out, size, "明天 還有 %lld 小時 %lld 分鐘", (long long)hours,
             (long long)(minutes % 60));
  } else {
    snprintf(out, size, "還有 %lld 天 %lld 小時", (long long)(hours / 24),
             (long long)(hours % 24));
  }
}

struct BatteryDisplay {
  bool warning;
  bool show_marker;
};

inline BatteryDisplay batteryDisplay(bool valid, int percent) {
  const bool low = valid && percent <= 20;
  return {low, low};
}

}  // namespace deviceui
