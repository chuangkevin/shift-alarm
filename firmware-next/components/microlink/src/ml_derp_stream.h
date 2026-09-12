#pragma once
#include <stddef.h>
#include <stdint.h>
/* read: positive bytes, 0 EOF, -2 temporary timeout/WANT, -1 fatal. */
typedef int (*ml_derp_read_fn)(void *,uint8_t *,size_t);
typedef uint64_t (*ml_derp_now_fn)(void *);
typedef void (*ml_derp_idle_fn)(void *);
static inline int ml_derp_read_header(void *ctx,ml_derp_read_fn read,
 ml_derp_now_fn now,ml_derp_idle_fn idle,uint8_t header[5],uint64_t timeout_ms){
 size_t used=0;uint64_t started=now(ctx);
 while(used<5){
  if(now(ctx)-started>=timeout_ms)return -1;
  int n=read(ctx,header+used,5-used);
  if(n==-2){if(!used)return 0;idle(ctx);continue;}
  if(n<=0||(size_t)n>5-used)return -1;
  used+=(size_t)n;
 }
 return 1;
}
