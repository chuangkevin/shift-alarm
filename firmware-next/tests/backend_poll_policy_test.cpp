#include "../main/backend_poll_policy.h"
#include <cassert>
#include <cstdint>

int main() {
  using namespace backendpoll;
  State state;
  observeConnectivity(state, true, 0x01020304);
  assert(canStart(state, 0, true, false));
  const Ticket first = begin(state, 0, 5000);
  enqueued(state);
  assert(blocksOta(state));
  assert(state.in_flight && !canStart(state, 5000, true, false));

  unsigned button_updates = 0;
  unsigned alarm_evaluations = 0;
  for (uint32_t now = 0; now <= 8000; now += 10) {
    ++button_updates;
    ++alarm_evaluations;
  }
  assert(button_updates == 801);
  assert(alarm_evaluations == 801);
  assert(accepts(state, first, false));
  finish(state, first);
  assert(!blocksOta(state));

  assert(canStart(state, 8000, true, false));
  const Ticket disconnected = begin(state, 8000, 5000);
  observeConnectivity(state, false, 0);
  observeConnectivity(state, true, 0x01020304);
  assert(!accepts(state, disconnected, false));
  finish(state, disconnected);

  const Ticket ota = begin(state, 13000, 5000);
  invalidate(state);
  assert(!accepts(state, ota, true));
  finish(state, ota);

  allocationFailed(state, 14000);
  assert(!canStart(state, 14000, true, false));
  assert(canStart(state, 14250, true, false));
  allocationFailed(state, 14250);
  assert(!canStart(state, 14749, true, false));
  assert(canStart(state, 14750, true, false));
  const Ticket recovered = begin(state, 14750, 5000);
  enqueued(state);
  assert(recovered.request_generation != ota.request_generation);
  finish(state, recovered);
  assert(state.allocation_backoff_ms == 0);
}
