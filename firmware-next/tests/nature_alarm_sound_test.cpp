#include "../main/nature_alarm_sound.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>

int main() {
  naturealarm::State state;
  for (int i = 0; i < 1000; ++i) assert(naturealarm::nextSample(state, false) == 0);

  int32_t peak = 0;
  int64_t ocean_energy = 0;
  int64_t bird_energy = 0;
  for (uint32_t i = 0; i < naturealarm::SAMPLE_RATE * 7u; ++i) {
    const int32_t sample = naturealarm::nextSample(state, true);
    const int32_t magnitude = std::abs(sample);
    if (magnitude > peak) peak = magnitude;
    if (i < naturealarm::SAMPLE_RATE) ocean_energy += magnitude;
    if (i >= naturealarm::SAMPLE_RATE * 2u && i < naturealarm::SAMPLE_RATE * 3u)
      bird_energy += magnitude;
  }
  assert(peak >= 1500);
  assert(peak <= 3600);
  assert(ocean_energy > 0);
  assert(bird_energy > ocean_energy * 2);

  assert(naturealarm::nextSample(state, false) == 0);
  assert(!state.active);
  assert(state.cycle_sample == 0);
  return 0;
}
