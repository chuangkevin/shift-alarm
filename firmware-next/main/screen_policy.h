#pragma once
#include <cstdint>

namespace screenpolicy {

constexpr uint32_t MILLIS_PER_MINUTE = 60UL * 1000;

inline bool validTimeout(uint16_t minutes) {
  return minutes == 0 || minutes == 1 || minutes == 5 || minutes == 15 ||
         minutes == 30 || minutes == 60;
}

inline bool shouldTurnOff(uint16_t minutes, uint32_t now, uint32_t lastActivity,
                          bool mustStayOn) {
  if (minutes == 0 || mustStayOn) return false;
  return uint32_t(now - lastActivity) >= uint32_t(minutes) * MILLIS_PER_MINUTE;
}

}  // namespace screenpolicy
