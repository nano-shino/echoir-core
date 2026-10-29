// The only translation unit that compiles miniaudio. Every other file includes
// audio/miniaudio_config.h for declarations only, so the implementation links once.
#define MINIAUDIO_IMPLEMENTATION
#include "audio/miniaudio_config.h"
