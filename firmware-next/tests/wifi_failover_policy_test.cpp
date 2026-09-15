#include "../main/wifi_failover_policy.h"
#include <cassert>
#include <cstdint>

int main() {
  using namespace wififailover;
  wifiprofiles::List profiles;
  for (const char *ssid : {"first", "second", "hidden", "last"})
    assert(wifiprofiles::addOrUpdate(profiles, {ssid, "password"}) == wifiprofiles::Result::Ok);
  std::array<Observation, wifiprofiles::MAX_PROFILES> seen{{{true, -70}, {true, -40}, {false, 0}, {true, -70}}};
  const Candidates ordered = candidates(profiles, seen);
  assert(ordered.count == 4);
  assert(ordered.indices[0] == 1);
  assert(ordered.indices[1] == 0 && ordered.indices[2] == 3);
  assert(ordered.indices[3] == 2);

  assert(!shouldStartCycle(true, false, 100, 0, 0));
  assert(!shouldStartCycle(false, true, 100, 0, 0));
  assert(shouldStartCycle(false, false, 2000, 0, 2000));
  assert(backoffMs(1) == 2000 && backoffMs(2) == 4000);
  assert(backoffMs(20) == BACKOFF_MAX_MS);
  const uint32_t before_wrap = UINT32_MAX - 1000;
  assert(!elapsed(998, before_wrap, 2000));
  assert(elapsed(999, before_wrap, 2000));
  assert(!elapsed(100, 50, CONNECT_TIMEOUT_MS));

  State state;
  beginScan(state, UINT32_MAX - 10);
  assert(state.phase == Phase::Scanning);
  scanned(state, ordered, UINT32_MAX - 5);
  assert(nextCandidate(state, UINT32_MAX - 4) == 1);
  assert(nextCandidate(state, UINT32_MAX - 3) == 0);
  assert(nextCandidate(state, UINT32_MAX - 2) == 3);
  assert(nextCandidate(state, UINT32_MAX - 1) == 2);
  assert(nextCandidate(state, 0) == -1);
  candidateFailed(state, 3);
  assert(state.phase == Phase::Idle && state.retry_delay_ms == 0);
  retryLater(state, 4);
  assert(state.phase == Phase::Backoff && state.retry_delay_ms == BACKOFF_INITIAL_MS);
  state.failed_cycles = 254;
  retryLater(state, UINT32_MAX - 100);
  assert(state.failed_cycles == 255 && state.retry_delay_ms == BACKOFF_MAX_MS);
  retryLater(state, 50);
  assert(state.failed_cycles == 255 && state.retry_delay_ms == BACKOFF_MAX_MS);
  assert(!shouldStartCycle(false, false, 59898, UINT32_MAX - 100, BACKOFF_MAX_MS));
  assert(shouldStartCycle(false, false, 59899, UINT32_MAX - 100, BACKOFF_MAX_MS));
  for (Phase phase : {Phase::Scanning, Phase::Connecting, Phase::Backoff}) {
    state.phase = phase; state.position = 3; state.failed_cycles = 200; state.retry_delay_ms = 60000;
    cancelForMutation(state, 77);
    assert(state.phase == Phase::Idle && state.position == 0 && state.failed_cycles == 0 && state.phase_started_ms == 77);
  }
  healthy(state);
  assert(state.phase == Phase::Idle && state.failed_cycles == 0);
}
