#include "../main/screen_policy.h"
#include <cassert>
#include <cstdint>

int main() {
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
