#pragma once
class JKRHeap;
struct JUTFont;
// Owns a resident copy of a disc BFN; deleting the font releases its storage.
// Returns nullptr on missing resources or allocation failure.
JUTFont* p2_load_resident_font(const char* archivePath,const char* resourceName,JKRHeap* heap);
