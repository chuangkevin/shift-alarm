#pragma once
#include <stdint.h>
namespace alarmclock {
constexpr int64_t VALID_CLOCK = 1704067200;
constexpr int64_t CATCHUP_SECONDS = 90;
constexpr int64_t SNOOZE_SECONDS = 300;
inline bool due(int64_t epoch, int64_t now, int64_t handled) {
  return now >= VALID_CLOCK && epoch > handled && epoch <= now && now - epoch <= CATCHUP_SECONDS;
}
inline bool upcoming(int64_t epoch, int64_t now, int64_t handled) {
  return epoch > handled && epoch > now;
}
}
