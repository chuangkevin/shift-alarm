#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "wifi_profiles.h"

namespace wififailover {

constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t DISCONNECT_SETTLE_MS = 300;
constexpr uint32_t BACKOFF_INITIAL_MS = 2000;
constexpr uint32_t BACKOFF_MAX_MS = 60000;

struct Observation {
  bool visible = false;
  int rssi = 0;
};

struct Candidates {
  std::array<uint8_t, wifiprofiles::MAX_PROFILES> indices{};
  size_t count = 0;
};

enum class Phase : uint8_t { Idle, Scanning, Connecting, Disconnecting, Backoff };

struct State {
  Phase phase = Phase::Idle;
  Candidates ordered{};
  size_t position = 0;
  uint8_t failed_cycles = 0;
  uint32_t phase_started_ms = 0;
  uint32_t retry_delay_ms = 0;
};

inline Candidates candidates(const wifiprofiles::List &profiles,
                             const std::array<Observation, wifiprofiles::MAX_PROFILES> &observations) {
  Candidates result;
  for (size_t i = 0; i < profiles.count; ++i) if (observations[i].visible) result.indices[result.count++] = uint8_t(i);
  for (size_t i = 1; i < result.count; ++i) {
    const uint8_t value = result.indices[i]; size_t j = i;
    while (j > 0 && observations[value].rssi > observations[result.indices[j - 1]].rssi) {
      result.indices[j] = result.indices[j - 1]; --j;
    }
    result.indices[j] = value;
  }
  for (size_t i = 0; i < profiles.count; ++i) if (!observations[i].visible) result.indices[result.count++] = uint8_t(i);
  return result;
}

inline bool elapsed(uint32_t now, uint32_t started, uint32_t duration) {
  return uint32_t(now - started) >= duration;
}

inline uint32_t backoffMs(uint8_t failures) {
  uint32_t delay = BACKOFF_INITIAL_MS;
  for (uint8_t i = 1; i < failures && delay < BACKOFF_MAX_MS; ++i) delay = delay > BACKOFF_MAX_MS / 2 ? BACKOFF_MAX_MS : delay * 2;
  return delay;
}

inline bool shouldStartCycle(bool connected, bool active, uint32_t now, uint32_t retry_started, uint32_t retry_delay) {
  return !connected && !active && elapsed(now, retry_started, retry_delay);
}

inline void beginScan(State &state, uint32_t now) {
  state.phase = Phase::Scanning; state.phase_started_ms = now; state.position = 0;
}

inline void scanned(State &state, const Candidates &ordered, uint32_t now) {
  state.ordered = ordered; state.position = 0; state.phase = Phase::Connecting; state.phase_started_ms = now;
}

inline int nextCandidate(State &state, uint32_t now) {
  if (state.position >= state.ordered.count) return -1;
  state.phase = Phase::Connecting; state.phase_started_ms = now;
  return state.ordered.indices[state.position++];
}

inline void waitForDisconnect(State &state, uint32_t now) {
  state.phase = Phase::Disconnecting; state.phase_started_ms = now;
}

inline bool disconnectSettled(const State &state, uint32_t now) {
  return state.phase == Phase::Disconnecting && elapsed(now, state.phase_started_ms, DISCONNECT_SETTLE_MS);
}

inline void retryLater(State &state, uint32_t now) {
  state.phase = Phase::Backoff; state.phase_started_ms = now;
  if (state.failed_cycles < UINT8_MAX) ++state.failed_cycles;
  state.retry_delay_ms = backoffMs(state.failed_cycles); state.position = 0;
}

inline void candidateFailed(State &state, uint32_t now) {
  state.phase = Phase::Idle; state.phase_started_ms = now; state.retry_delay_ms = 0; state.position = 0;
}

inline void healthy(State &state) {
  state.phase = Phase::Idle; state.failed_cycles = 0; state.retry_delay_ms = 0; state.position = 0;
}

inline void cancelForMutation(State &state, uint32_t now) {
  state = {}; state.phase_started_ms = now;
}

}  // namespace wififailover
