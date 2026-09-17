#pragma once

#include <stdint.h>

namespace naturealarm {

constexpr uint32_t SAMPLE_RATE = 24000;
constexpr uint32_t BIRD_CYCLE_SAMPLES = SAMPLE_RATE * 6;

struct State {
  uint32_t noise = 0x4d595df4u;
  int32_t ocean_lpf = 0;
  uint32_t bird_phase = 0;
  uint32_t cycle_sample = 0;
  bool active = false;
};

inline void reset(State &state) {
  state = State{};
}

inline int16_t sine64(uint32_t phase) {
  static constexpr int16_t table[64] = {
      0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
      23170, 25330, 27245, 28898, 30273, 31356, 32137, 32609,
      32767, 32609, 32137, 31356, 30273, 28898, 27245, 25330,
      23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
      0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
      -23170, -25330, -27245, -28898, -30273, -31356, -32137, -32609,
      -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25330,
      -23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212};
  return table[phase >> 26];
}

inline uint16_t triangleEnvelope(uint32_t position, uint32_t period) {
  const uint32_t half = period / 2;
  const uint32_t ramp = position < half ? position : period - position;
  return static_cast<uint16_t>(8192u + (ramp * 24575u) / half);
}

inline int32_t birdChirp(State &state, uint32_t position, uint32_t start,
                         uint32_t duration, uint32_t from_hz, uint32_t to_hz) {
  if (position < start || position >= start + duration) return 0;
  const uint32_t p = position - start;
  const uint32_t frequency = from_hz + ((to_hz - from_hz) * p) / duration;
  state.bird_phase += frequency * 178957u;  // 2^32 / 24000, rounded.
  const uint32_t attack = p < 480u ? p : 480u;
  const uint32_t remaining = duration - p;
  const uint32_t release = remaining < 960u ? remaining : 960u;
  const uint32_t envelope = static_cast<uint32_t>(
      (static_cast<uint64_t>(attack) * release * 32767u) / (480u * 960u));
  return static_cast<int32_t>(
      (static_cast<int64_t>(sine64(state.bird_phase)) * envelope * 2600) /
      (32767LL * 32767LL));
}

inline int16_t nextSample(State &state, bool ringing) {
  if (!ringing) {
    if (state.active) reset(state);
    return 0;
  }
  state.active = true;

  // Filtered noise with an eight-second swell gives a quiet surf bed.
  state.noise ^= state.noise << 13;
  state.noise ^= state.noise >> 17;
  state.noise ^= state.noise << 5;
  const int32_t noise = static_cast<int16_t>(state.noise >> 16);
  state.ocean_lpf += (noise - state.ocean_lpf) >> 4;
  const uint32_t swell_position = state.cycle_sample % (SAMPLE_RATE * 8u);
  const uint16_t swell = triangleEnvelope(swell_position, SAMPLE_RATE * 8u);
  int32_t sample = (state.ocean_lpf * static_cast<int32_t>(swell)) / (32767 * 10);

  // Two short upward calls every six seconds. Their soft attack/release avoids clicks.
  sample += birdChirp(state, state.cycle_sample, SAMPLE_RATE * 2u, 4320u, 1750u, 3150u);
  sample += birdChirp(state, state.cycle_sample, SAMPLE_RATE * 2u + 10800u, 3600u, 2050u, 3450u);

  state.cycle_sample = (state.cycle_sample + 1u) % BIRD_CYCLE_SAMPLES;
  if (sample > 3600) sample = 3600;
  if (sample < -3600) sample = -3600;
  return static_cast<int16_t>(sample);
}

}  // namespace naturealarm
