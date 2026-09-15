#pragma once
#include <stdint.h>

namespace backendpoll {

constexpr uint32_t ALLOCATION_BACKOFF_MIN_MS = 250;
constexpr uint32_t ALLOCATION_BACKOFF_MAX_MS = 5000;

struct Ticket {
  uint32_t request_generation;
  uint32_t validity_generation;
};

struct State {
  uint32_t request_generation = 0;
  uint32_t validity_generation = 1;
  uint32_t network_ip = 0;
  uint32_t next_attempt_ms = 0;
  uint32_t allocation_backoff_ms = 0;
  bool network_ready = false;
  bool in_flight = false;
};

inline bool elapsed(uint32_t now, uint32_t deadline) {
  return int32_t(now - deadline) >= 0;
}

inline void invalidate(State &state) {
  ++state.validity_generation;
  if (state.validity_generation == 0) ++state.validity_generation;
}

inline void observeConnectivity(State &state, bool ready, uint32_t ip) {
  if (state.network_ready != ready || (ready && state.network_ip != ip)) invalidate(state);
  state.network_ready = ready;
  state.network_ip = ready ? ip : 0;
}

inline bool canStart(const State &state, uint32_t now, bool transport_ready, bool ota_busy) {
  return state.network_ready && transport_ready && !ota_busy && !state.in_flight &&
         elapsed(now, state.next_attempt_ms);
}
inline bool blocksOta(const State &state) { return state.in_flight; }

inline Ticket begin(State &state, uint32_t now, uint32_t interval_ms) {
  state.in_flight = true;
  ++state.request_generation;
  if (state.request_generation == 0) ++state.request_generation;
  state.next_attempt_ms = now + interval_ms;
  return {state.request_generation, state.validity_generation};
}

inline void enqueued(State &state) { state.allocation_backoff_ms = 0; }

inline bool accepts(const State &state, Ticket ticket, bool ota_busy) {
  return state.in_flight && state.network_ready && !ota_busy &&
         ticket.request_generation == state.request_generation &&
         ticket.validity_generation == state.validity_generation;
}

inline void finish(State &state, Ticket ticket) {
  if (ticket.request_generation == state.request_generation) state.in_flight = false;
}

inline void finishGeneration(State &state, uint32_t request_generation) {
  if (request_generation == state.request_generation) state.in_flight = false;
}

inline void allocationFailed(State &state, uint32_t now) {
  state.in_flight = false;
  state.allocation_backoff_ms = state.allocation_backoff_ms == 0
                                    ? ALLOCATION_BACKOFF_MIN_MS
                                    : state.allocation_backoff_ms * 2;
  if (state.allocation_backoff_ms > ALLOCATION_BACKOFF_MAX_MS)
    state.allocation_backoff_ms = ALLOCATION_BACKOFF_MAX_MS;
  state.next_attempt_ms = now + state.allocation_backoff_ms;
}

}  // namespace backendpoll
