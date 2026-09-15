#include "../main/ui_policy.h"
#include <cassert>
#include <cstring>
#include <stdint.h>

int main() {
  using namespace deviceui;
  State state;
  right(state, 10);
  assert(state.page == Page::Menu && state.selection == 1);
  right(state, 20);
  assert(state.selection == 2);
  center(state, 30);
  assert(state.page == Page::Schedule);
  right(state, 40);
  assert(state.page == Page::Device);
  center(state, 50);
  assert(state.page == Page::Menu && state.selection == 3);
  state.selection = 5;
  center(state, 60);
  assert(state.page == Page::Main);
  state.selection = 1;
  center(state, 70);
  assert(state.page == Page::Connectivity);

  state.page = Page::Device;
  state.last_interaction_ms = UINT32_MAX - 10000;
  assert(!returnIfInactive(state, 4998));
  assert(returnIfInactive(state, 5000) && state.page == Page::Main);

  char text[96];
  countdown(1704067200, 0, text, sizeof(text));
  assert(strcmp(text, "尚無下一次鬧鐘") == 0);
  countdown(1704067200, 1704067200 + 90 * 60, text, sizeof(text));
  assert(strcmp(text, "今天 還有 1 小時 30 分鐘") == 0);
  const int64_t taipei_2330 = 1704123000;
  countdown(taipei_2330, taipei_2330 + 60 * 60, text, sizeof(text));
  assert(strncmp(text, "明天", strlen("明天")) == 0);
  countdown(1704067200, 1704067200 + 2 * 86400 + 3 * 3600, text, sizeof(text));
  assert(strcmp(text, "還有 2 天 3 小時") == 0);

  assert(!batteryDisplay(false, 0).warning);
  assert(batteryDisplay(true, 20).warning && batteryDisplay(true, 20).show_marker);
  assert(!batteryDisplay(true, 21).warning);

  assert(qrQuietRight(PHONE_QR_X, QR_VERSION_8_MODULES, PHONE_QR_SCALE) == 113);
  assert(PHONE_TEXT_X - qrQuietRight(PHONE_QR_X, QR_VERSION_8_MODULES,
                                     PHONE_QR_SCALE) >= PHONE_QR_SCALE * 4);
}
