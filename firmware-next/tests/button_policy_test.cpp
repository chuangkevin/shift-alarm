#include "../main/button_policy.h"
#include <cassert>
#include <stdint.h>

using buttons::Event;

static Event tick(buttons::State &s, uint32_t now, bool l, bool c, bool r,
                  bool awake = true, bool ringing = false) {
  return buttons::update(s, now, l, c, r, awake, ringing);
}

int main() {
  buttons::State state;
  assert(tick(state, 0, false, false, true) == Event::None);
  assert(tick(state, 29, false, false, true) == Event::None);
  assert(tick(state, 30, false, false, true) == Event::None);
  assert(tick(state, 31, false, false, false) == Event::None);
  assert(tick(state, 61, false, false, false) == Event::RightShort);

  assert(tick(state, 100, false, true, false) == Event::None);
  assert(tick(state, 130, false, true, false) == Event::None);
  assert(tick(state, 1330, false, false, false) == Event::None);
  assert(tick(state, 1360, false, false, false) == Event::CenterLong);

  buttons::State wake;
  tick(wake, 0, true, false, false, false);
  assert(tick(wake, 30, true, false, false, false) == Event::Wake);
  tick(wake, 31, false, false, false, true);
  assert(tick(wake, 61, false, false, false, true) == Event::None);

  buttons::State wake_chord;
  tick(wake_chord, 0, true, false, true, false);
  assert(tick(wake_chord, 30, true, false, true, false) == Event::Wake);
  assert(tick(wake_chord, 10030, true, false, true) == Event::None);
  tick(wake_chord, 10031, false, false, false);
  assert(tick(wake_chord, 10061, false, false, false) == Event::None);

  buttons::State ring;
  tick(ring, 0, false, true, false);
  tick(ring, 30, false, true, false);
  buttons::beginRinging(ring, 30, false, true, false);
  assert(tick(ring, 31, false, true, false, true, true) == Event::None);
  tick(ring, 32, false, false, false, true, true);
  tick(ring, 62, false, false, false, true, true);
  assert(tick(ring, 63, false, true, false, true, true) == Event::StopAlarm);
  buttons::endRinging(ring);
  tick(ring, 64, false, false, false);
  assert(tick(ring, 94, false, false, false) == Event::None);

  buttons::State held_at_transition;
  buttons::beginRinging(held_at_transition, 100, true, false, false);
  assert(tick(held_at_transition, 100, true, false, false, true, true) == Event::None);
  tick(held_at_transition, 101, false, false, false, true, true);
  tick(held_at_transition, 131, false, false, false, true, true);
  assert(tick(held_at_transition, 132, true, false, false, true, true) == Event::StopAlarm);

  buttons::State debouncing_release;
  tick(debouncing_release, 0, false, true, false);
  tick(debouncing_release, 30, false, true, false);
  tick(debouncing_release, 31, false, false, false);
  buttons::beginRinging(debouncing_release, 31, false, false, false);
  assert(tick(debouncing_release, 32, false, true, false, true, true) == Event::None);
  tick(debouncing_release, 33, false, false, false, true, true);
  tick(debouncing_release, 63, false, false, false, true, true);
  assert(tick(debouncing_release, 64, false, true, false, true, true) == Event::StopAlarm);

  buttons::State ringing_chord;
  tick(ringing_chord, 0, true, false, true);
  tick(ringing_chord, 30, true, false, true);
  buttons::beginRinging(ringing_chord, 30, true, false, true);
  assert(tick(ringing_chord, 10030, true, false, true, true, true) == Event::None);
  tick(ringing_chord, 10031, false, false, false, true, true);
  tick(ringing_chord, 10061, false, false, false, true, true);
  assert(tick(ringing_chord, 10062, true, false, false, true, true) == Event::StopAlarm);

  buttons::State chord;
  tick(chord, 0, true, false, true);
  tick(chord, 30, true, false, true);
  assert(buttons::pairingSecondsRemaining(chord, 30) == 10);
  assert(buttons::pairingSecondsRemaining(chord, 9029) == 2);
  assert(tick(chord, 10030, true, false, true) == Event::StartPairing);
  assert(buttons::pairingSecondsRemaining(chord, 10031) == 0);
  assert(buttons::pairingAllowed(Event::StartPairing, false));
  assert(!buttons::pairingAllowed(Event::StartPairing, true));
  assert(tick(chord, 20030, true, false, true) == Event::None);
  tick(chord, 20031, false, false, false);
  assert(tick(chord, 20061, false, false, false) == Event::None);

  buttons::State canceled_chord;
  tick(canceled_chord, 0, true, false, true);
  tick(canceled_chord, 30, true, false, true);
  tick(canceled_chord, 5000, false, false, true);
  assert(!canceled_chord.chord);
  tick(canceled_chord, 5030, false, false, true);
  tick(canceled_chord, 5100, true, false, true);
  tick(canceled_chord, 5130, true, false, true);
  tick(canceled_chord, 5140, false, false, true);
  assert(tick(canceled_chord, 5170, false, false, true) == Event::None);
  tick(canceled_chord, 5171, false, false, false);
  assert(tick(canceled_chord, 5201, false, false, false) == Event::None);
  tick(canceled_chord, 6000, true, false, true);
  tick(canceled_chord, 6030, true, false, true);
  assert(tick(canceled_chord, 16029, true, false, true) == Event::None);
  assert(tick(canceled_chord, 16030, true, false, true) == Event::StartPairing);

  buttons::State chord_rollover;
  tick(chord_rollover, UINT32_MAX - 10020, true, false, true);
  tick(chord_rollover, UINT32_MAX - 9990, true, false, true);
  assert(buttons::pairingSecondsRemaining(chord_rollover, 8) == 1);
  assert(tick(chord_rollover, 9, true, false, true) == Event::StartPairing);
  assert(buttons::pairingSecondsRemaining(chord_rollover, UINT32_MAX) == 0);

  // A raw alarm-stop tap shorter than debounce must not poison the next press.
  buttons::State quick_stop;
  buttons::beginRinging(quick_stop, 99, false, false, false);
  assert(tick(quick_stop, 100, true, false, false, true, true) == Event::StopAlarm);
  buttons::endRinging(quick_stop);
  assert(tick(quick_stop, 110, false, false, false) == Event::None);
  tick(quick_stop, 200, true, false, false);
  tick(quick_stop, 230, true, false, false);
  tick(quick_stop, 240, false, false, false);
  assert(tick(quick_stop, 270, false, false, false) == Event::LeftShort);

  buttons::State rollover;
  tick(rollover, UINT32_MAX - 20, false, true, false);
  tick(rollover, 10, false, true, false);
  tick(rollover, 11, false, false, false);
  assert(tick(rollover, 41, false, false, false) == Event::CenterShort);
}
