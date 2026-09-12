#include "ml_control_stream.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static unsigned allocations,fail_at;
static void *alloc(size_t n){return ++allocations==fail_at?NULL:malloc(n);}
static bool reject(void *ctx,const char *p,size_t n){(void)ctx;(void)p;(void)n;return false;}
static unsigned frames,maps;static size_t bytes[8];
static bool frame(void *ctx,uint8_t type,uint8_t flags,uint32_t stream,const uint8_t *p,size_t n){(void)ctx;(void)flags;(void)p;frames++;if(type==0&&stream<8)bytes[stream]+=n;return true;}
static bool map(void *ctx,const char *p,size_t n){(void)ctx;assert(n==2&&!memcmp(p,"{}",2)&&!p[n]);maps++;return true;}
static size_t h2(uint8_t *p,unsigned stream,const uint8_t *data,size_t n,unsigned flags){memset(p,0,9);p[1]=n>>8;p[2]=n;p[4]=flags;p[8]=stream;if(n)memcpy(p+9,data,n);return n+9;}
int main(void){
 const uint8_t record[]={2,0,0,0,'{','}'};uint8_t wire[200];size_t n=0;
 n+=h2(wire+n,5,record,3,0);n+=h2(wire+n,7,(const uint8_t *)"xx",2,0);
 n+=h2(wire+n,5,record+3,3,0);uint8_t two[12];memcpy(two,record,6);memcpy(two+6,record,6);n+=h2(wire+n,5,two,12,0);
 for(size_t chunk=1;chunk<=n;chunk++) {
  ml_control_stream_t s;ml_control_stream_init(&s,malloc);maps=frames=0;memset(bytes,0,sizeof(bytes));
  for(size_t i=0;i<n;i+=chunk){size_t take=n-i;if(take>chunk)take=chunk;assert(ml_control_stream_feed(&s,wire+i,take,frame,map,NULL));}
  assert(maps==3&&frames==4&&bytes[5]==18&&bytes[7]==2);assert(!s.frame&&!s.map);ml_control_stream_reset(&s);
 }
 ml_control_stream_t s;ml_control_stream_init(&s,malloc);maps=0;
 uint8_t padded[]={2,2,0,0,0,'{','}',0,0};n=h2(wire,5,padded,sizeof(padded),8);
 assert(ml_control_stream_feed(&s,wire,n,frame,map,NULL)&&maps==1);
 n=h2(wire,5,record,6,1);assert(!ml_control_stream_feed(&s,wire,n,frame,map,NULL));ml_control_stream_reset(&s);
 n=h2(wire,5,(const uint8_t *)"\0\0\0\0",4,0);assert(!ml_control_stream_feed(&s,wire,n,frame,map,NULL));ml_control_stream_reset(&s);
 uint8_t huge[]={1,0,4,0};n=h2(wire,5,huge,4,0);assert(!ml_control_stream_feed(&s,wire,n,frame,map,NULL));ml_control_stream_reset(&s);
 memset(wire,0,9);wire[1]=64;wire[2]=1;assert(!ml_control_stream_feed(&s,wire,9,frame,map,NULL));ml_control_stream_reset(&s);
 n=h2(wire,5,record,2,0);assert(ml_control_stream_feed(&s,wire,n,frame,map,NULL));ml_control_stream_reset(&s);assert(!s.prefix_used&&!s.header_used&&!s.map);
 n=h2(wire,5,record,6,0);assert(ml_control_stream_feed(&s,wire,n,frame,map,NULL));ml_control_stream_reset(&s);
 for(fail_at=1;fail_at<=2;fail_at++) {
  allocations=0;ml_control_stream_init(&s,alloc);n=h2(wire,5,record,6,0);
  assert(!ml_control_stream_feed(&s,wire,n,frame,map,NULL));ml_control_stream_reset(&s);
 }
 ml_control_stream_init(&s,malloc);n=h2(wire,5,record,6,0);
 assert(!ml_control_stream_feed(&s,wire,n,frame,reject,NULL));ml_control_stream_reset(&s);
 memset(wire,0,9);wire[3]=7;assert(!ml_control_stream_feed(&s,wire,9,frame,map,NULL));ml_control_stream_reset(&s);
 memset(wire,0,9);wire[3]=3;wire[8]=5;assert(!ml_control_stream_feed(&s,wire,9,frame,map,NULL));ml_control_stream_reset(&s);
 puts("PASS: H2 splits, map-prefix/body splits, multiple maps, interleaved streams, padding, limits, END_STREAM and reconnect reset");
}
