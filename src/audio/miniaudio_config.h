// The one place that configures miniaudio. Include this header, never miniaudio.h, so every
// translation unit sees the same macros and the same struct layouts.
//
// Core scope: decoding and resampling. The engine, node graph, resource manager and signal
// generators are off. Device I/O stays on, because the CLI captures audio with it.
#pragma once

#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION

#include "miniaudio.h"
