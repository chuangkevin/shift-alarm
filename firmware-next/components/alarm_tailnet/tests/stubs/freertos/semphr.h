#pragma once
#include <stdlib.h>
typedef void *SemaphoreHandle_t;
static inline void *xSemaphoreCreateMutex(void){return malloc(1);}
static inline void xSemaphoreTake(void *s,int timeout){(void)s;(void)timeout;}
static inline void xSemaphoreGive(void *s){(void)s;}
static inline void vSemaphoreDelete(void *s){free(s);}
