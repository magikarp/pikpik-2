#pragma once
#include <cstddef>

// Validate the byte extent and lookup tables consumed by the JPAC2-10 loader.
// This never allocates or changes the caller's data. Error strings are static.
bool p2_validate_jpc(const void* data,std::size_t size,const char** error=nullptr);
