#pragma once
// =====================================================================
//  pdmp2_port  --  where the Layer II encoder's working set lives
// ---------------------------------------------------------------------
//  pdmp2 asks for about 35 KB at open() and nothing after. On this badge
//  that must not come out of internal SRAM: the main task has an 8.5 KB
//  stack and internal RAM is the scarce thing, while PSRAM is 32 MB and
//  idle. The encoder touches these buffers once per audio frame (52 ms),
//  so PSRAM's latency is irrelevant to it.
//
//  Reached by -DPDMP2_CONFIG_H='"pdmp2_port.h"' in CMakeLists.txt.
// =====================================================================

#include "esp_heap_caps.h"

#define PDMP2_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define PDMP2_FREE(p)   heap_caps_free((p))
