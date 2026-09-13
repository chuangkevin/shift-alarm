#pragma once
#include <cstdint>

namespace screenpolicy {

constexpr uint32_t MILLIS_PER_MINUTE = 60UL * 1000;

inline bool validTimeout(uint16_t minutes) {
  return minutes == 0 || minutes == 1 || minutes == 5 || minutes == 15 ||
         minutes == 30 || minutes == 60;
}

inline bool validBrightness(uint8_t percent) {
  return percent == 10 || percent == 25 || percent == 40 || percent == 60 ||
         percent == 80 || percent == 100;
}

inline uint8_t brightnessDuty(uint8_t percent) {
  return uint8_t((uint16_t(percent) * 255 + 50) / 100);
}

inline bool shouldTurnOff(uint16_t minutes, uint32_t now, uint32_t lastActivity,
                          bool mustStayOn) {
  if (minutes == 0 || mustStayOn) return false;
  return uint32_t(now - lastActivity) >= uint32_t(minutes) * MILLIS_PER_MINUTE;
}

}  // namespace screenpolicy
