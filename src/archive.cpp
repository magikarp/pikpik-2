#include "p2_assets.h"
#include "p2_endian.h"
#include "p2_memory.h"
#include "p2_game_alloc.h"
#include "Dolphin/dvd.h"
#include "JSystem/JKernel/JKRArchive.h"
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace {
u32 word(const u8* p) { u32 v; std::memcpy(&v, p, 4); return p2_big_endian(v); }
u16 half(const u8* p) { u16 v; std::memcpy(&v, p, 2); return p2_big_endian(v); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

JKRArchive::JKRArchive() : JKRArchive(-1, EMM_Unk0) {}

// Disk entries are 20 bytes; runtime entries contain genuine 64-bit pointers.
// Keep the data payload immutable and relocate metadata into the owning heap.
bool JKRMemArchive::open(void* buffer, u32 size, JKRMemBreakFlag flag) {
    mHeader = nullptr; mDataInfo = nullptr; mDirectories = nullptr;
    mFileEntries = nullptr; mExpandSizes = nullptr; mStrTable = nullptr;
    mArchiveData = nullptr; mIsOpen = false; mCompression = COMPRESSION_None;
    try {
        require(buffer && size >= 64, "short archive header");
        auto* b = static_cast<u8*>(buffer);
        const size_t length = word(b+4), base = word(b+8);
        require(length <= size && length >= 64 && base <= length-32, "archive header bounds");
        {
            // Validation needs a transient full copy; keep it out of the game
            // heap, which is sized for the archive alone.
            P2HostScratchScope scratch;
            p2::Bytes bytes(b, b+length);
            p2::readRarc(bytes); // Validates the tree and all reachable payloads.
        }
        const u32 dirs = word(b+base), files = word(b+base+8);
        const size_t dirAt = base+word(b+base+4), fileAt = base+word(b+base+12);
        const size_t strAt = base+word(b+base+20), strSize = word(b+base+16);
        auto nameValid = [&](u32 at) { return at < strSize && std::memchr(b+strAt+at, 0, strSize-at); };
        for (u32 i=0; i<dirs; ++i) {
            auto* d = b+dirAt+i*16;
            require(nameValid(word(d+4)), "archive directory name");
            require(word(d+12) <= files && half(d+10) <= files-word(d+12), "archive directory entries");
        }
        for (u32 i=0; i<files; ++i) {
            auto* f = b+fileAt+i*20;
            require(nameValid(word(f+4)&0xffffff), "archive file name");
            if (!(word(f+4)&0x02000000))
                require(word(f+8) <= word(b+16) && word(f+12) <= word(b+16)-word(f+8), "archive file bounds");
            else require(word(f+8) == UINT32_MAX || word(f+8) < dirs, "archive child directory");
        }
        const size_t tableSize = sizeof(SArcDataInfo)+size_t(dirs)*sizeof(SDIDirEntry)+size_t(files)*sizeof(SDIFileEntry);
        require(tableSize <= UINT32_MAX, "archive metadata too large");
        mDataInfo = static_cast<SArcDataInfo*>(mHeap->alloc(tableSize, 8));
        require(mDataInfo != nullptr, "archive metadata allocation");
        *mDataInfo = {};
        mDataInfo->mNumDirEntries = dirs; mDataInfo->mNumFileEntries = files;
        mDataInfo->mStrTableLength = strSize;
        mDataInfo->mNextFreeFileID = half(b+base+24); mDataInfo->mIsSyncIDs = b[base+26] != 0;
        mDirectories = reinterpret_cast<SDIDirEntry*>(mDataInfo+1);
        mFileEntries = reinterpret_cast<SDIFileEntry*>(mDirectories+dirs);
        for (u32 i=0; i<dirs; ++i) {
            auto* d = b+dirAt+i*16;
            mDirectories[i] = {word(d), word(d+4), half(d+8), half(d+10), word(d+12)};
        }
        for (u32 i=0; i<files; ++i) {
            auto* f = b+fileAt+i*20;
            mFileEntries[i] = {half(f), half(f+2), word(f+4), word(f+8), word(f+12), nullptr};
        }
        mHeader = static_cast<SArcHeader*>(buffer); mStrTable = reinterpret_cast<char*>(b+strAt);
        mArchiveData = b+base+word(b+12); mIsOpen = flag == MBF_1;
        return true;
    } catch (const std::exception& error) {
        p2_heap_reportf("Cannot mount RARC: %s\n", error.what());
        mMountMode = EMM_Unk0;
        return false;
    }
}

bool JKRMemArchive::open(s32 entry, EMountDirection direction) {
    mHeader = nullptr; mDataInfo = nullptr; mIsOpen = false;
    mMountDirection = direction;
    void* owned = nullptr;
    try {
        P2HostScratchScope scratch; // Transient read/decompress buffers only.
        DVDFileInfo file = {};
        require(DVDFastOpen(entry, &file), "archive disc entry");
        p2::Bytes compressed(file.length);
        const s32 read = DVDReadPrio(&file, compressed.data(), file.length, 0, 2);
        DVDClose(&file);
        require(read == s32(compressed.size()), "archive disc read");
        auto bytes = p2::decompressResource(compressed);
        owned = mHeap->alloc(bytes.size(), direction == EMD_Tail ? -32 : 32);
        require(owned != nullptr, "archive payload allocation");
        std::memcpy(owned, bytes.data(), bytes.size());
        if (open(owned, bytes.size(), MBF_1)) return true;
    } catch (const std::exception& error) {
        p2_heap_reportf("Cannot load archive: %s\n", error.what());
    }
    if (owned) mHeap->free(owned);
    mMountMode = EMM_Unk0;
    return false;
}

u32 JKRMemArchive::fetchResource_subroutine(u8* source, u32 length, u8* dest, u32 capacity, int compression) {
    if (compression == COMPRESSION_None) {
        u32 count = std::min(length, capacity); std::memcpy(dest, source, count); return count;
    }
    try {
        require(compression == COMPRESSION_YAZ0 || compression == COMPRESSION_YAY0, "unsupported resource compression");
        require(length >= 16 && !std::memcmp(source, compression == COMPRESSION_YAZ0 ? "Yaz0" : "Yay0", 4),
                "resource compression flag disagrees with header");
        auto bytes = p2::decompressResource(p2::Bytes(source, source+length));
        u32 count = std::min<size_t>(bytes.size(), capacity); std::memcpy(dest, bytes.data(), count); return count;
    } catch (const std::exception& error) {
        p2_heap_reportf("Cannot expand archive resource: %s\n", error.what()); return 0;
    }
}
