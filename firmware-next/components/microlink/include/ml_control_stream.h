#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ML_CONTROL_FRAME_MAX 16384u
#define ML_CONTROL_MAP_MAX 262144u
typedef bool (*ml_control_frame_fn)(void *,uint8_t,uint8_t,uint32_t,const uint8_t *,size_t);
typedef bool (*ml_control_map_fn)(void *,const char *,size_t);
typedef struct {
    void *(*alloc)(size_t);
    uint8_t header[9],prefix[4];
    size_t header_used,frame_used,frame_len,prefix_used,map_used,map_len;
    uint8_t *frame;
    char *map;
} ml_control_stream_t;
void ml_control_stream_init(ml_control_stream_t *,void *(*alloc)(size_t));
void ml_control_stream_reset(ml_control_stream_t *);
bool ml_control_stream_feed(ml_control_stream_t *,const uint8_t *,size_t,
                           ml_control_frame_fn,ml_control_map_fn,void *);
