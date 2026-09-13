#include "../main/screen_policy.h"
#include <cassert>
#include <cstdint>

int main() {
  assert(screenpolicy::validBrightness(10));
  assert(screenpolicy::validBrightness(40));
  assert(screenpolicy::validBrightness(100));
  assert(!screenpolicy::validBrightness(0));
  assert(!screenpolicy::validBrightness(50));
  assert(screenpolicy::brightnessDuty(10) == 26);
  assert(screenpolicy::brightnessDuty(40) == 102);
  assert(screenpolicy::brightnessDuty(100) == 255);
  using screenpolicy::shouldTurnOff;
  using screenpolicy::validTimeout;

  assert(validTimeout(0));
  assert(validTimeout(1));
  assert(validTimeout(60));
  assert(!validTimeout(2));
  assert(!validTimeout(65535));

  assert(!shouldTurnOff(0, 900000, 0, false));
  assert(!shouldTurnOff(1, 59999, 0, false));
  assert(shouldTurnOff(1, 60000, 0, false));
  assert(!shouldTurnOff(1, 60000, 0, true));

  // Unsigned subtraction keeps the timeout correct across millis() rollover.
  const uint32_t beforeWrap = UINT32_MAX - 30000;
  const uint32_t afterWrap = 30000;
  assert(shouldTurnOff(1, afterWrap, beforeWrap, false));
  return 0;
}
