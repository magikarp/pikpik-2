#pragma once
#include <stdint.h>
#include <stddef.h>
bool p2_aram_read(uint32_t address,void* destination,size_t size);
bool p2_aram_write(uint32_t address,const void* source,size_t size);
// Direct read-only view for the audio mixer, which reads ARAM as the DSP did
// (no copy, no lock); null outside backing memory.
const unsigned char* p2_aram_host(uint32_t address,size_t size);
