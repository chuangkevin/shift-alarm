#include "../src/scheduler.h"
#include <cassert>
int main() {
  using namespace alarmclock;
  int64_t t=1800000000;
  assert(due(t,t,0)); assert(due(t,t+90,0));
  assert(!due(t,t+91,0)); assert(!due(t,t-1,0));
  assert(!due(t,t,t)); assert(!due(0,0,0));
  assert(!due(t,t-10,t)); // Backward clock cannot replay a handled alarm.
  assert(upcoming(t+1,t,0)); assert(!upcoming(t,t,t));
}
