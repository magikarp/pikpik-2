#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRFileFinder.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

static void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main(int argc, const char** argv) {
    require(argc == 2, "disc root argument");
    require(p2_memory_init(128*1024*1024), "native arena");
    auto* heap = JKRExpHeap::createRoot(16, false);
    require(heap && p2_dvd_mount(argv[1]), "heap and DVD mount");
    u8 yay[]={'Y','a','y','0', 0,0,0,6, 0,0,0,20, 0,0,0,22,
              0xe0,0,0,0, 0x10,2, 'a','b','c'};
    u8 decoded[8]; std::memset(decoded,0x5a,sizeof(decoded));
    require(JKRMemArchive::fetchResource_subroutine(yay,sizeof(yay),decoded,4,COMPRESSION_YAY0)==4 &&
            !std::memcmp(decoded,"abca",4) && decoded[4]==0x5a,
            "archive Yay0 expansion honors caller capacity");
    require(JKRMemArchive::fetchResource_subroutine(yay,sizeof(yay),decoded,4,COMPRESSION_YAZ0)==0,
            "archive compression flag must agree with header");
    // Initialize the DVD service's persistent handle table before measuring
    // per-archive ownership (closing a handle retains its hash table capacity).
    DVDFileInfo warmup = {};
    require(DVDOpen(const_cast<char*>("new_screen/eng/title.szs"), &warmup), "DVD handle initialization");
    DVDClose(&warmup);
    size_t verified = 0;
    for (const char* path : {"user/Kando/piki/pikis.szs", "new_screen/eng/title.szs"}) {
        auto bytes = p2::decompressYaz0(p2::readAsset(std::filesystem::path(argv[1])/"files", path));
        auto resources = p2::readRarc(bytes);
        const u32 freeBefore = heap->getTotalFreeSize();
        for (auto mode : {JKRArchive::EMM_Mem, JKRArchive::EMM_Dvd, JKRArchive::EMM_Aram, JKRArchive::EMM_Comp}) {
            auto* archive = JKRArchive::mount(path, mode, heap, JKRArchive::EMD_Head);
            require(archive != nullptr, "disc archive mount");
            require(JKRArchive::mount(path, mode, heap, JKRArchive::EMD_Head) == archive, "mount identity reuse");
            archive->unmount();
            for (const auto& resource : resources) {
                const auto name = "/"+resource.path;
                auto* data = archive->getResource(name.c_str());
                require(data && archive->getResSize(data) == resource.size, "original lookup and size");
                require(!std::memcmp(data, bytes.data()+resource.offset, resource.size), "resource bytes match disc");
                const auto count = std::min<size_t>(resource.size, 37);
                unsigned char prefix[37];
                require(archive->readResource(prefix, count, name.c_str()) == count, "bounded resource read");
                require(!std::memcmp(prefix, data, count), "readResource bytes");
                ++verified;
            }
            require(!archive->getResource("/missing-resource"), "missing resource");
            auto* finder = archive->getFirstFile("/");
            require(finder && finder->mIsAvailable, "directory enumeration");
            delete finder;
            archive->unmount();
            require(heap->check() && heap->getTotalFreeSize() == freeBefore, "archive unmount releases storage");
        }
        auto* memoryArchive = JKRArchive::mount(bytes.data(), heap, JKRArchive::EMD_Head);
        require(memoryArchive && memoryArchive->countFile(), "memory archive mount");
        memoryArchive->unmount();
        require(heap->getTotalFreeSize() == freeBefore, "borrowed archive leaves source owned by caller");
        auto* truncated = new (heap, 8) JKRMemArchive(bytes.data(), 32, MBF_0);
        require(truncated->getMountMode() == JKRArchive::EMM_Unk0, "reject truncated archive");
        delete truncated;
        const u8 signature = bytes[0]; bytes[0] = 0;
        require(!JKRArchive::mount(bytes.data(), heap, JKRArchive::EMD_Head), "reject invalid archive mount");
        bytes[0] = signature;
        require(heap->getTotalFreeSize() == freeBefore, "failed archive mounts release storage");
    }
    std::printf("Original JKR archive APIs verified %zu resource loads and balanced unmounts.\n", verified);
}
