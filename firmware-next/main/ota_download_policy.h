#pragma once
#include <stdint.h>
namespace alarm_download {
constexpr uint32_t total_ms = 600000;
constexpr uint32_t idle_ms = 30000;
enum class Deadline { none, total, idle };
// Unsigned elapsed arithmetic also covers millis() rollover.
inline Deadline deadline(uint32_t now, uint32_t started, uint32_t progressed) {
    if (uint32_t(now - started) >= total_ms) return Deadline::total;
    if (uint32_t(now - progressed) >= idle_ms) return Deadline::idle;
    return Deadline::none;
}
}
