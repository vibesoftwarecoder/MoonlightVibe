#pragma once
// Test stub: only what nvhttp.cpp needs to compile. Not the real library.
#include <cstdint>

typedef struct _STREAM_CONFIGURATION {
    int width;
    int height;
    int fps;
    int supportedVideoFormats;
    int audioConfiguration;
    char remoteInputAesKey[16];
    char remoteInputAesIv[16];
} STREAM_CONFIGURATION, *PSTREAM_CONFIGURATION;

#define VIDEO_FORMAT_MASK_10BIT 0x2200
#define SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(x) (x)

inline const char* LiGetLaunchUrlQueryParameters() { return ""; }
