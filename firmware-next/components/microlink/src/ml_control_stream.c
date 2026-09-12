#include "ml_control_stream.h"
#include <stdlib.h>
#include <string.h>
void ml_control_stream_init(ml_control_stream_t *s,void *(*alloc)(size_t)) {
    memset(s,0,sizeof(*s));s->alloc=alloc;
}
void ml_control_stream_reset(ml_control_stream_t *s) {
    void *(*alloc)(size_t)=s->alloc;free(s->frame);free(s->map);
    ml_control_stream_init(s,alloc);
}
static bool maps(ml_control_stream_t *s,const uint8_t *p,size_t n,ml_control_map_fn cb,void *ctx) {
    while(n) {
        if(s->prefix_used<4) {
            size_t take=4-s->prefix_used;if(take>n)take=n;
            memcpy(s->prefix+s->prefix_used,p,take);s->prefix_used+=take;p+=take;n-=take;
            if(s->prefix_used<4)continue;
            s->map_len=(uint32_t)s->prefix[0]|((uint32_t)s->prefix[1]<<8)|((uint32_t)s->prefix[2]<<16)|((uint32_t)s->prefix[3]<<24);
            if(!s->map_len||s->map_len>ML_CONTROL_MAP_MAX)return false;
            s->map=s->alloc(s->map_len+1);if(!s->map)return false;
        }
        size_t take=s->map_len-s->map_used;if(take>n)take=n;
        memcpy(s->map+s->map_used,p,take);s->map_used+=take;p+=take;n-=take;
        if(s->map_used==s->map_len) {
            s->map[s->map_len]=0;
            if(!cb(ctx,s->map,s->map_len))return false;
            free(s->map);s->map=NULL;s->prefix_used=s->map_used=s->map_len=0;
        }
    }
    return true;
}
static bool complete(ml_control_stream_t *s,ml_control_frame_fn frame_cb,ml_control_map_fn map_cb,void *ctx) {
    uint8_t type=s->header[3],flags=s->header[4];
    uint32_t stream=((uint32_t)(s->header[5]&127)<<24)|((uint32_t)s->header[6]<<16)|((uint32_t)s->header[7]<<8)|s->header[8];
    if(!frame_cb(ctx,type,flags,stream,s->frame,s->frame_len))return false;
    if(type==0) {
        if(!stream)return false;
        size_t offset=0,len=s->frame_len;
        if(flags&8) {if(!len||s->frame[0]>=len)return false;offset=1;len-=1+s->frame[0];}
        if(stream==5&&len&&!maps(s,s->frame+offset,len,map_cb,ctx))return false;
    }
    /* A closed/reset map stream needs a fresh authoritative fetch. */
    if(type==7||(stream==5&&((type==0||type==1)&&(flags&1)))||(stream==5&&type==3))return false;
    free(s->frame);s->frame=NULL;s->header_used=s->frame_used=s->frame_len=0;
    return true;
}
bool ml_control_stream_feed(ml_control_stream_t *s,const uint8_t *p,size_t n,
                           ml_control_frame_fn frame_cb,ml_control_map_fn map_cb,void *ctx) {
    while(n) {
        if(s->header_used<9) {
            size_t take=9-s->header_used;if(take>n)take=n;
            memcpy(s->header+s->header_used,p,take);s->header_used+=take;p+=take;n-=take;
            if(s->header_used<9)continue;
            s->frame_len=((uint32_t)s->header[0]<<16)|((uint32_t)s->header[1]<<8)|s->header[2];
            if(s->frame_len>ML_CONTROL_FRAME_MAX)return false;
            if(s->frame_len){s->frame=s->alloc(s->frame_len);if(!s->frame)return false;}
        }
        size_t take=s->frame_len-s->frame_used;if(take>n)take=n;
        if(take)memcpy(s->frame+s->frame_used,p,take);
        s->frame_used+=take;p+=take;n-=take;
        if(s->frame_used==s->frame_len&&!complete(s,frame_cb,map_cb,ctx))return false;
    }
    return true;
}
