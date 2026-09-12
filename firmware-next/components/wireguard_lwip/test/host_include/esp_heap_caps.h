#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_calloc(n,s,c) calloc(n,s)
#define heap_caps_free(p) free(p)
