#pragma once
// Host stand-in for esp_heap_caps.h: every capability is plain malloc.
// A host has no PSRAM/SRAM split, and a checker does not care which one
// the badge would have used -- only that the code runs.
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM             (1u << 10)
#define MALLOC_CAP_INTERNAL           (1u << 11)
#define MALLOC_CAP_8BIT               (1u << 2)
#define MALLOC_CAP_DEFAULT            (1u << 3)
#define heap_caps_malloc(n, caps)     malloc(n)
#define heap_caps_calloc(k, n, caps)  calloc((k), (n))
#define heap_caps_realloc(p, n, caps) realloc((p), (n))
#define heap_caps_free(p)             free(p)
