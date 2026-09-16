#pragma once
#include <stdint.h>
namespace wifiindicator {
enum class State : uint8_t { Offline, Connecting, Connected };
struct Value { State state; uint8_t bars; };
constexpr int X=116, Y=5, WIDTH=30, HEIGHT=22;
inline Value value(bool connected, bool trying, int rssi) {
  if (!connected) return {trying?State::Connecting:State::Offline,0};
  // RSSI zero is not a valid sample from a connected station.
  if (rssi>=0 || rssi < -100) return {State::Connected,0};
  return {State::Connected,static_cast<uint8_t>(rssi>=-55?4:rssi>=-67?3:rssi>=-75?2:1)};
}
inline bool changed(Value a,Value b){return a.state!=b.state||a.bars!=b.bars;}
static_assert(X>102 && X+WIDTH<154 && Y+HEIGHT<37,"indicator must fit between date, battery and clock");
}
