#pragma once
#include <stdint.h>

namespace buttons {

constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t WAKE_ARM_RELEASE_MS = 1000;
constexpr uint32_t WAKE_HOLD_MS = 700;
constexpr uint32_t LONG_PRESS_MS = 1200;
constexpr uint32_t PAIRING_HOLD_MS = 10000;

enum Event : uint8_t {
  None = 0,
  LeftShort = 1,
  CenterShort = 2,
  RightShort = 4,
  CenterLong = 8,
  Wake = 16,
  StopAlarm = 32,
  StartPairing = 64,
};

struct Key {
  bool raw = false;
  bool stable = false;
  bool suppressed = false;
  uint32_t raw_changed_ms = 0;
  uint32_t pressed_ms = 0;
};

struct State {
  Key left;
  Key center;
  Key right;
  bool chord = false;
  bool chord_lockout = false;
  bool pairing_sent = false;
  bool wake_sent = false;
  bool screen_was_awake = true;
  bool wake_armed = false;
  bool wake_release_tracking = false;
  bool wake_press_ready = false;
  uint8_t wake_candidate_mask = 0;
  uint32_t wake_candidate_ms = 0;
  uint32_t wake_release_ms = 0;
  bool ringing_active = false;
  uint8_t previous_raw_mask = 0;
  uint8_t ring_blocked_mask = 0;
  uint32_t chord_started_ms = 0;
};

inline uint8_t rawMask(const State &state) {
  return (state.left.raw ? 1 : 0) | (state.center.raw ? 2 : 0) |
         (state.right.raw ? 4 : 0);
}

inline uint8_t heldMask(const State &state) {
  return ((state.left.raw || state.left.stable) ? 1 : 0) |
         ((state.center.raw || state.center.stable) ? 2 : 0) |
         ((state.right.raw || state.right.stable) ? 4 : 0);
}

inline void suppress(State &state, uint8_t mask) {
  state.left.suppressed |= mask & 1;
  state.center.suppressed |= mask & 2;
  state.right.suppressed |= mask & 4;
}

inline void beginRinging(State &state, uint32_t now, bool left, bool center, bool right) {
  if (state.left.raw != left) state.left.raw_changed_ms = now;
  if (state.center.raw != center) state.center.raw_changed_ms = now;
  if (state.right.raw != right) state.right.raw_changed_ms = now;
  state.left.raw = left;
  state.center.raw = center;
  state.right.raw = right;
  state.ringing_active = true;
  state.ring_blocked_mask = heldMask(state);
  state.previous_raw_mask = rawMask(state);
  suppress(state, state.ring_blocked_mask);
  state.chord = false;
  state.chord_lockout = state.ring_blocked_mask != 0;
  state.pairing_sent = false;
}

inline void endRinging(State &state) {
  state.ringing_active = false;
  state.ring_blocked_mask = 0;
}

inline uint8_t pairingSecondsRemaining(const State &state, uint32_t now) {
  if (!state.chord || state.pairing_sent) return 0;
  const uint32_t elapsed = uint32_t(now - state.chord_started_ms);
  if (elapsed >= PAIRING_HOLD_MS) return 0;
  return uint8_t((PAIRING_HOLD_MS - elapsed + 999) / 1000);
}

inline bool pairingAllowed(Event event, bool ringing) {
  return event == StartPairing && !ringing;
}

inline bool debounce(Key &key, bool raw, uint32_t now) {
  if (raw != key.raw) {
    key.raw = raw;
    key.raw_changed_ms = now;
  }
  if (key.stable == key.raw || uint32_t(now - key.raw_changed_ms) < DEBOUNCE_MS) return false;
  key.stable = key.raw;
  if (key.stable) key.pressed_ms = now;
  return true;
}

inline Event update(State &state, uint32_t now, bool left, bool center, bool right,
                    bool screen_awake, bool ringing) {
  if (ringing && !state.ringing_active) beginRinging(state, now, left, center, right);
  else if (!ringing && state.ringing_active) endRinging(state);

  const uint8_t raw_mask = (left ? 1 : 0) | (center ? 2 : 0) | (right ? 4 : 0);
  const uint8_t rising = raw_mask & ~state.previous_raw_mask;
  const uint8_t falling = state.previous_raw_mask & ~raw_mask;
  state.previous_raw_mask = raw_mask;
  if ((falling & 1) && !state.left.stable) state.left.suppressed = false;
  if ((falling & 2) && !state.center.stable) state.center.suppressed = false;
  if ((falling & 4) && !state.right.stable) state.right.suppressed = false;

  if (state.chord && !(left && right)) {
    state.chord = false;
    state.pairing_sent = false;
    state.chord_lockout = true;
    suppress(state, 5);
  }
  const bool left_edge = debounce(state.left, left, now);
  const bool center_edge = debounce(state.center, center, now);
  const bool right_edge = debounce(state.right, right, now);
  const bool any = state.left.stable || state.center.stable || state.right.stable;
  if (!left && !state.left.stable) state.ring_blocked_mask &= ~uint8_t(1);
  if (!center && !state.center.stable) state.ring_blocked_mask &= ~uint8_t(2);
  if (!right && !state.right.stable) state.ring_blocked_mask &= ~uint8_t(4);

  if (ringing) {
    const uint8_t stop_edge = rising & ~state.ring_blocked_mask;
    if (stop_edge) {
      state.chord = false;
      state.chord_lockout = true;
      state.pairing_sent = false;
      suppress(state, stop_edge);
      return StopAlarm;
    }
    return None;
  }

  if (!screen_awake) {
    const uint8_t stable_mask = (state.left.stable ? 1 : 0) |
                                (state.center.stable ? 2 : 0) |
                                (state.right.stable ? 4 : 0);
    if (state.screen_was_awake) {
      state.screen_was_awake = false;
      state.wake_sent = false;
      state.wake_armed = false;
      state.wake_release_tracking = false;
      state.wake_press_ready = false;
      state.wake_candidate_mask = 0;
    }
    if (!raw_mask && !stable_mask) {
      if (state.wake_press_ready) {
        state.wake_sent = true;
        state.wake_armed = false;
        state.wake_press_ready = false;
        state.wake_release_tracking = false;
        state.wake_candidate_mask = 0;
        return Wake;
      }
      if (!state.wake_release_tracking) {
        state.wake_release_tracking = true;
        state.wake_release_ms = now;
      } else if (uint32_t(now - state.wake_release_ms) >= WAKE_ARM_RELEASE_MS) {
        state.wake_armed = true;
      }
      state.wake_candidate_mask = 0;
      return None;
    }
    state.wake_release_tracking = false;
    if (!state.wake_armed) return None;
    // GPIO0 is the dedicated middle/BOOT button.  Once all keys have been
    // released long enough to arm wake-up, its raw press edge is deliberate
    // and must wake even when the user taps faster than the debounce window.
    if (rising & 2) {
      state.wake_sent = true;
      state.wake_armed = false;
      state.wake_press_ready = false;
      state.wake_candidate_mask = 0;
      state.center.suppressed = true;
      return Wake;
    }
    if (state.wake_candidate_mask != stable_mask) {
      state.wake_candidate_mask = stable_mask;
      state.wake_candidate_ms = now;
      return None;
    }
    if (stable_mask && !state.wake_press_ready &&
        uint32_t(now - state.wake_candidate_ms) >= WAKE_HOLD_MS) {
      state.wake_press_ready = true;
      state.left.suppressed |= state.left.stable;
      state.center.suppressed |= state.center.stable;
      state.right.suppressed |= state.right.stable;
    }
    return None;
  }
  state.screen_was_awake = true;
  state.wake_armed = false;
  state.wake_release_tracking = false;
  state.wake_press_ready = false;
  state.wake_candidate_mask = 0;
  if (!any) state.wake_sent = false;

  if (state.chord_lockout) {
    if (left || right || state.left.stable || state.right.stable) {
      suppress(state, 5);
      return None;
    }
    state.chord_lockout = false;
    state.left.suppressed = state.right.suppressed = false;
    return None;
  }

  const bool chord_now = left && right && state.left.stable && state.right.stable &&
                         !state.chord_lockout &&
                         (state.chord || (!state.left.suppressed && !state.right.suppressed));
  if (chord_now) {
    state.left.suppressed = state.right.suppressed = true;
    if (!state.chord) {
      state.chord = true;
      state.pairing_sent = false;
      state.chord_started_ms = now;
    }
    if (!state.pairing_sent && uint32_t(now - state.chord_started_ms) >= PAIRING_HOLD_MS) {
      state.pairing_sent = true;
      return StartPairing;
    }
    return None;
  }
  if (state.chord) {
    if (!state.left.stable && !state.right.stable) {
      state.chord = false;
      state.left.suppressed = state.right.suppressed = false;
    }
    return None;
  }

  if (left_edge && !state.left.stable) {
    const bool suppressed = state.left.suppressed;
    state.left.suppressed = false;
    if (!suppressed) return LeftShort;
  }
  if (right_edge && !state.right.stable) {
    const bool suppressed = state.right.suppressed;
    state.right.suppressed = false;
    if (!suppressed) return RightShort;
  }
  if (center_edge && !state.center.stable) {
    const bool suppressed = state.center.suppressed;
    state.center.suppressed = false;
    if (!suppressed)
      return uint32_t(now - state.center.pressed_ms) >= LONG_PRESS_MS ? CenterLong : CenterShort;
  }
  return None;
}

}  // namespace buttons
