#pragma once
#include <stdlib.h>
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return malloc(1);}
static inline int xSemaphoreTake(SemaphoreHandle_t mutex,int ticks){(void)mutex;(void)ticks;return 1;}
static inline void xSemaphoreGive(SemaphoreHandle_t mutex){(void)mutex;}
