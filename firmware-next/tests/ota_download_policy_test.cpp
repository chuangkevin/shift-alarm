#include "../main/ota_download_policy.h"
#include <cassert>
int main() {
    using namespace alarm_download;
    // A slow stream progresses for five minutes, beyond the previous deadline.
    for(uint32_t t=0;t<=300000;t+=1000) assert(deadline(t,0,t)==Deadline::none);
    assert(deadline(29999,0,0)==Deadline::none);
    assert(deadline(30000,0,0)==Deadline::idle);
    assert(deadline(599999,0,599999)==Deadline::none);
    assert(deadline(600000,0,600000)==Deadline::total);
    // Progress resets only idle timeout, never the total budget.
    assert(deadline(599999,0,570000)==Deadline::none);
    assert(deadline(600000,0,570000)==Deadline::total);
    uint32_t start=UINT32_MAX-10000;
    assert(deadline(start+29999,start,start)==Deadline::none);
    assert(deadline(start+30000,start,start)==Deadline::idle);
    assert(deadline(start+600000,start,start+600000)==Deadline::total);
    assert(mayReconnect(0));
    assert(mayReconnect(2));
    assert(!mayReconnect(3));
}
