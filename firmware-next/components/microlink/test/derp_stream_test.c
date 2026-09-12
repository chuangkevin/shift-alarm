#include "../src/ml_derp_stream.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef struct{int steps[16];size_t step,offset;uint64_t clock;uint8_t bytes[5];} Fake;
static int read_fake(void *ctx,uint8_t *buf,size_t len){Fake *f=ctx;int n=f->steps[f->step++];if(n>0){assert((size_t)n<=len);memcpy(buf,f->bytes+f->offset,n);f->offset+=(size_t)n;}return n;}
static uint64_t now_fake(void *ctx){return ((Fake*)ctx)->clock;}
static void idle_fake(void *ctx){((Fake*)ctx)->clock+=1000;}
int main(void){uint8_t h[5];
 Fake whole={.steps={5},.bytes={5,0,0,1,2}};
 assert(ml_derp_read_header(&whole,read_fake,now_fake,idle_fake,h,2000)==1);assert(!memcmp(h,whole.bytes,5));
 Fake fragmented={.steps={1,-2,1,2,1},.bytes={5,0,0,1,2}};
 assert(ml_derp_read_header(&fragmented,read_fake,now_fake,idle_fake,h,2000)==1);assert(!memcmp(h,fragmented.bytes,5));
 Fake idle={.steps={-2}};assert(ml_derp_read_header(&idle,read_fake,now_fake,idle_fake,h,2000)==0);
 Fake eof={.steps={0}};assert(ml_derp_read_header(&eof,read_fake,now_fake,idle_fake,h,2000)==-1);
 Fake cut={.steps={2,0}};assert(ml_derp_read_header(&cut,read_fake,now_fake,idle_fake,h,2000)==-1);
 Fake slow={.steps={1,-2,-2}};assert(ml_derp_read_header(&slow,read_fake,now_fake,idle_fake,h,2000)==-1);assert(slow.clock==2000);
 puts("DERP fake TLS whole/fragmented/WANT/idle/EOF/deadline PASS");
}
