#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Only the DERP owner task mutates these slots. Home never enters this cache. */
#define ML_DERP_REMOTE_SLOTS 2
#define ML_DERP_IDLE_MS 60000ULL
typedef struct {
    uint16_t region;
    unsigned failures;
    uint64_t last_used_ms, retry_after_ms;
} ml_derp_cache_slot_t;
static inline int ml_derp_cache_find(const ml_derp_cache_slot_t *slots,
                                     uint16_t region, uint64_t now, bool demand) {
    if(!region)return -1;
    for(unsigned i=0;i<ML_DERP_REMOTE_SLOTS;i++)if(slots[i].region==region)return (int)i;
    if(!demand||!region)return -1;
    int oldest=-1;
    for(unsigned i=0;i<ML_DERP_REMOTE_SLOTS;i++) {
        if(!slots[i].region)return (int)i;
        if(now>=slots[i].retry_after_ms && now>=slots[i].last_used_ms &&
           now-slots[i].last_used_ms>=ML_DERP_IDLE_MS &&
           (oldest<0||slots[i].last_used_ms<slots[oldest].last_used_ms))oldest=(int)i;
    }
    return oldest;
}
static inline void ml_derp_cache_failure(ml_derp_cache_slot_t *slot,uint64_t now) {
    if(slot->failures<6)slot->failures++;
    uint64_t delay=1000ULL<<(slot->failures-1);
    if(delay>30000)delay=30000;
    slot->retry_after_ms=now+delay;
}
