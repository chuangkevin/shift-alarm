#pragma once
#include <stdint.h>

static inline uint16_t alarm_tailnet_retry_seconds(uint8_t failures) {
    if (failures == 0) return 0;
    return failures < 6 ? (uint16_t)(1U << (failures - 1)) : 30;
}
