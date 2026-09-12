#include "../main/scheduler.h"
#include <cassert>
#include <iostream>
int main(){
 using namespace alarmclock;
 const int64_t today=VALID_CLOCK+86400*100;
 const int64_t first=today+86400,second=today+86400*2;
 assert(due(second,second,0));
 int64_t handled=second; // Incorrect future clock already rang the second alarm.
 handled=reconcileHandled(today,handled);
 assert(handled==today);
 assert(upcoming(first,today,handled)&&upcoming(second,today,handled));
 assert(due(first,first,handled));
 handled=first;
 // Small NTP corrections, including the exact boundary, must not replay.
 for(int64_t delta:{0LL,1LL,90LL}){
  assert(reconcileHandled(first-delta,handled)==handled);
  assert(!due(first,first,reconcileHandled(first-delta,handled)));
 }
 assert(reconcileHandled(first-91,handled)==first-91);
 assert(reconcileHandled(first+1,handled)==handled);
 assert(reconcileHandled(0,handled)==handled); // Unset boot clock cannot reset NVS.
 std::cout<<"Clock rollback recovery and replay protection passed\n";
}
