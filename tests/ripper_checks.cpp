#include "JSystem/JKernel/JKRDvdRipper.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>

static void require(bool value, const char* what) {
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
int main(int argc, const char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024), "disc and native arena");
    auto* heap=JKRExpHeap::createRoot(16,false);
    require(heap && p2_dvd_mount(argv[1]), "heap and DVD");
    for(const char* path : {"user/Ebisawa/title/title.szs", "user/Ebisawa/title/bg_spring.szs",
                           "user/Ebisawa/title/bg_summer.szs", "user/Ebisawa/title/bg_autumn.szs",
                           "user/Ebisawa/title/bg_winter.szs"}) {
        const auto raw=p2::readAsset(std::filesystem::path(argv[1])/"files",path);
        const auto expanded=p2::decompressResource(raw);
        const auto entry=DVDConvertPathToEntrynum(const_cast<char*>(path));
        require(entry>=0 && expanded.size()>256, "real title asset");
        u32 baseline=0;
        for(unsigned iteration=0;iteration<3;++iteration) {
            {
                int kind=-1; u32 size=0;
                auto* data=JKRDvdRipper::loadToMainRAM(path,nullptr,Switch_1,0,heap,
                    JKRDvdRipper::ALLOC_DIR_TOP,0,&kind,&size);
                require(data && size==expanded.size() && kind==COMPRESSION_YAZ0 &&
                    !std::memcmp(data,expanded.data(),size), "path loader decodes actual title archive");
                require((reinterpret_cast<uintptr_t>(data)&31)==0 && JKRHeap::findFromRoot(data)==heap,
                    "expanded resource is aligned and owned by requested game heap");
                // Feed the ripper's output directly into the real archive reader.
                require(!p2::readRarc(p2::Bytes(static_cast<u8*>(data),static_cast<u8*>(data)+size)).empty(),
                    "loaded data contains the real title resource tree");
                JKRHeap::free(data,heap);
                data=JKRDvdRipper::loadToMainRAM(entry,nullptr,Switch_0,0,heap,
                    JKRDvdRipper::ALLOC_DIR_BOTTOM,0,&kind,&size);
                require(data && kind==COMPRESSION_None && size==((raw.size()+31)&~size_t(31)) &&
                    !std::memcmp(data,raw.data(),raw.size()), "entry loader preserves raw bytes and padded length");
                for(size_t i=raw.size();i<size;++i) require(static_cast<u8*>(data)[i]==0,"raw tail padding is zero");
                JKRHeap::free(data,heap);
                JKRDvdFile file(entry);
                alignas(32) u8 buffer[160]; std::memset(buffer,0xa5,sizeof(buffer));
                data=JKRDvdRipper::loadToMainRAM(&file,buffer,Switch_1,128,nullptr,
                    JKRDvdRipper::ALLOC_DIR_BOTTOM,93,&kind,&size);
                require(data==buffer && size==128 && kind==COMPRESSION_YAZ0 &&
                    !std::memcmp(buffer,expanded.data()+93,128),"decoded offset and caller capacity");
                for(size_t i=128;i<sizeof(buffer);++i) require(buffer[i]==0xa5,"caller buffer limit preserved");
                data=JKRDvdRipper::loadToMainRAM(&file,buffer,Switch_2,128,nullptr,
                    JKRDvdRipper::ALLOC_DIR_TOP,32,&kind,&size);
                require(data==buffer && size==128 && kind==COMPRESSION_None &&
                    !std::memcmp(buffer,raw.data()+32,128),"no-expand raw offset");
                data=JKRDvdRipper::loadToMainRAM(&file,buffer,Switch_1,128,nullptr,
                    JKRDvdRipper::ALLOC_DIR_TOP,expanded.size()-17,&kind,&size);
                require(data==buffer && size==17 && !std::memcmp(buffer,expanded.data()+expanded.size()-17,17),
                    "decoded EOF clips requested output");
                std::memset(buffer,0x5a,sizeof(buffer));
                require(!JKRDvdRipper::loadToMainRAM(&file,buffer,Switch_1,128,nullptr,
                    JKRDvdRipper::ALLOC_DIR_TOP,UINT_MAX,&kind,&size) && size==0,
                    "invalid offset fails without underflow");
                for(auto byte:buffer) require(byte==0x5a,"failed read leaves destination intact");
                require(JKRDecompressFromDVD(&file,buffer,raw.size(),128,71,0,&size)==0 && size==128 &&
                    !std::memcmp(buffer,expanded.data()+71,128),"direct DVD decompression entry point");
                require(JKRDecompressFromDVD(&file,buffer,20,128,0,0,&size)==-1 && size==0,
                    "truncated compressed stream fails");
                file.close();
                require(!JKRDvdRipper::loadToMainRAM(&file,buffer,Switch_1,128,nullptr,
                    JKRDvdRipper::ALLOC_DIR_TOP,0,&kind,&size),"closed file fails promptly");
            }
            if(!iteration) baseline=heap->getTotalFreeSize();
            else require(heap->getTotalFreeSize()==baseline,"ripper allocations and file handles cleaned up");
        }
    }
    require(p2_dvd_mount(argv[1]),"all loader file handles released");
    std::puts("JKRDvdRipper: five real title archives, all overloads, both directions, raw/decoded offsets, bounds and cleanup pass.");
}
