#define MINIAUDIO_IMPLEMENTATION

#define STB_VORBIS_HEADER_ONLY /* stb_vorbis's declarations half, so miniaudio's Vorbis block sees the types */
#include "stb_vorbis.c"

#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY /* stb_vorbis's implementation half, above miniaudio's calls into it */
#include "stb_vorbis.c"
