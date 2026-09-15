#pragma once
#include <stdint.h>
#include <stdio.h>

namespace deviceidentity {

inline void suffix(const uint8_t mac[6], char out[5]) {
  snprintf(out, 5, "%02X%02X", mac[4], mac[5]);
}

}  // namespace deviceidentity
