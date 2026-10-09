#include "JSystem/JKernel/JKRDvdRipper.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_game_alloc.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

JSUList<JKRDMCommand> JKRDvdRipper::sDvdAsyncList;
bool JKRDvdRipper::errorRetry = true;
int JKRDvdRipper::sSZSBufferSize = 0x400;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int compression(const p2::Bytes& bytes, size_t offset = 0) {
    if (offset > bytes.size() || bytes.size()-offset < 4) return COMPRESSION_None;
    if (!std::memcmp(bytes.data()+offset, "Yaz0", 4)) return COMPRESSION_YAZ0;
    if (!std::memcmp(bytes.data()+offset, "Yay0", 4)) return COMPRESSION_YAY0;
    return COMPRESSION_None;
}
p2::Bytes readFile(JKRDvdFile* file, u32 begin = 0, u32 limit = 0) {
    require(file && file->mFileOpen, "DVD file is not open");
    const u32 length = file->getFileSize();
    require(length <= 256*1024*1024, "DVD file exceeds the bring-up limit");
    const u32 end = limit ? std::min(length, limit) : length;
    require(begin <= end, "DVD source offset exceeds file");
    p2::Bytes bytes(end-begin);
    if (!bytes.empty())
        require(file->readData(bytes.data(), bytes.size(), begin) == s32(bytes.size()), "DVD resource read failed");
    return bytes;
}
size_t outputCount(const p2::Bytes& bytes, u32 offset, u32 capacity) {
    require(offset <= bytes.size(), "DVD decoded offset exceeds resource");
    const size_t remaining = bytes.size()-offset;
    return capacity ? std::min<size_t>(remaining, capacity) : remaining;
}
}

// The original path and FST-entry wrappers still construct JKRDvdFile and call
// this overload. Host I/O completes synchronously; malformed reads fail instead
// of entering the console's infinite retrace/retry loop.
void* JKRDvdRipper::loadToMainRAM(JKRDvdFile* file, u8* destination,
    JKRExpandSwitch expand, u32 capacity, JKRHeap* heap, EAllocDirection direction,
    u32 offset, int* detectedCompression, u32* transferred) {
    if (detectedCompression) *detectedCompression = COMPRESSION_None;
    if (transferred) *transferred = 0;
    P2HostScratchScope scratch;
    try {
        auto bytes = readFile(file);
        const int kind = expand == Switch_1 ? compression(bytes) : COMPRESSION_None;
        if (detectedCompression) *detectedCompression = kind;
        if (kind != COMPRESSION_None) {
            bytes = p2::decompressResource(bytes);
            // For a compressed file, offset counts decoded bytes.
        } else if (expand == Switch_1 && offset && compression(bytes, offset) != COMPRESSION_None) {
            // Embedded resources use a raw file offset, then start decoding at
            // byte zero. The API reports compression of the outer file only.
            bytes = p2::decompressResource(p2::Bytes(bytes.begin()+offset, bytes.end()));
            offset = 0;
        } else {
            // Raw reads retain the SDK's zero-filled final 32-byte alignment.
            bytes.resize((bytes.size()+31)&~size_t(31), 0);
        }
        const auto count = outputCount(bytes, offset, capacity);
        if (!destination) {
            destination = static_cast<u8*>(JKRHeap::alloc(std::max<size_t>(count, 1),
                direction == ALLOC_DIR_TOP ? 32 : -32, heap));
            if (!destination) return nullptr;
        }
        if (count) std::memcpy(destination, bytes.data()+offset, count);
        if (transferred) *transferred = count;
        return destination;
    } catch (const std::exception& error) {
        p2_heap_reportf("Cannot load DVD resource: %s\n", error.what());
        return nullptr;
    }
}

int JKRDecompressFromDVD(JKRDvdFile* file, void* destination, u32 sourceLimit,
    u32 capacity, u32 decodedOffset, u32 sourceOffset, u32* transferred) {
    if (transferred) *transferred = 0;
    P2HostScratchScope scratch;
    try {
        require(destination && sourceOffset <= sourceLimit, "Invalid DVD decompression range");
        auto bytes = readFile(file, sourceOffset, sourceLimit);
        require(compression(bytes) == COMPRESSION_YAZ0, "Expected a DVD Yaz0 resource");
        bytes = p2::decompressResource(bytes);
        const auto count = outputCount(bytes, decodedOffset, capacity);
        if (count) std::memcpy(destination, bytes.data()+decodedOffset, count);
        if (transferred) *transferred = count;
        return 0;
    } catch (const std::exception& error) {
        p2_heap_reportf("Cannot decompress DVD resource: %s\n", error.what());
        return -1;
    }
}
