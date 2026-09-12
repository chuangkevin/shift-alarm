#include "../include/ml_derp_cache.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 ml_derp_cache_slot_t slots[2]={{0}};
 assert(ml_derp_cache_find(slots,0,100,true)==-1);
 assert(ml_derp_cache_find(slots,20,100,false)==-1);
 assert(ml_derp_cache_find(slots,20,100,true)==0);
 slots[0]=(ml_derp_cache_slot_t){.region=20,.last_used_ms=100};
 assert(ml_derp_cache_find(slots,20,200,false)==0);
 assert(ml_derp_cache_find(slots,3,200,true)==1);
 slots[1]=(ml_derp_cache_slot_t){.region=3,.last_used_ms=200};
 assert(ml_derp_cache_find(slots,4,300,true)==-1);
 assert(ml_derp_cache_find(slots,4,60100,false)==-1);
 assert(ml_derp_cache_find(slots,4,60100,true)==0);
 slots[0].retry_after_ms=90000;
 assert(ml_derp_cache_find(slots,4,60200,true)==1);
 ml_derp_cache_slot_t fail={0};uint64_t expected[]={1000,2000,4000,8000,16000,30000,30000,30000};
 for(unsigned i=0;i<8;i++){ml_derp_cache_failure(&fail,100);assert(fail.retry_after_ms==100+expected[i]);}
 puts("actual DERP cache: region reuse, two-slot capacity, demand-only allocation, idle LRU, retry backoff PASS");
}
