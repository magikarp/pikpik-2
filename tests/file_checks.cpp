#include "JSystem/JKernel/JKRFile.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JSupport/JSUStream.h"
#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <thread>

static void require(bool value,const char* what) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
int main(int argc,const char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024),"disc and memory");
    auto* heap=JKRExpHeap::createRoot(16,false);
    require(heap && p2_dvd_mount(argv[1]),"heap and native DVD");
    const char* path="user/Ebisawa/title/title.szs";
    const auto reference=p2::readAsset(std::filesystem::path(argv[1])/"files",path);
    const s32 entry=DVDConvertPathToEntrynum(const_cast<char*>(path));
    require(entry>=0 && reference.size()>512,"real title archive");
    u32 baseline=0;
    for(unsigned iteration=0;iteration<16;++iteration) {
        {
            JKRDvdFile file(path);
            require(file.mFileOpen && file.getFileSize()==reference.size(),"original JKR file opens real title archive");
            require(file.mDvdPlayer.mFile==&file,"native callback owns a real derived file-info object");
            alignas(32) unsigned char data[256]={};
            require(file.readData(data,sizeof(data),0)==sizeof(data) && !std::memcmp(data,reference.data(),sizeof(data)),"original JKR completion queue returns file bytes");
            require(file.readData(data,32,-1)==DVD_RESULT_FATAL_ERROR,"negative completion result survives pointer message round trip");
            require(file.readDataAsync(data,32,32)==32 && !std::memcmp(data,reference.data()+32,32),"original inline read path handles completion");
            JSUFileInputStream stream(&file);
            require(stream.readU32()==0x59617a30,"original file stream reads big-endian Yaz0 magic");
            require(stream.readData(data,-1)==0 && stream.getPosition()==4,"negative file stream read leaves position unchanged");
            stream.seekPos(INT_MAX,SEEK_CUR);
            require(stream.getPosition()==file.getFileSize() && stream.readData(data,1)==0,"large seek clamps at EOF");
            stream.seekPos(INT_MIN,SEEK_CUR);
            require(stream.getPosition()==0,"large negative seek clamps at start");
            stream.seekPos(13,SEEK_END);
            require(stream.readData(data,sizeof(data))==13 && !std::memcmp(data,reference.data()+reference.size()-13,13),"file stream clips reads to EOF");
            auto worker=[&](s32 offset) {
                alignas(32) unsigned char read[32];
                for(unsigned n=0;n<20;++n)
                    require(file.readData(read,32,offset)==32 && !std::memcmp(read,reference.data()+offset,32),"concurrent JKR reads serialize completion correctly");
            };
            std::thread first(worker,64), second(worker,128); first.join(); second.join();
            file.close(); require(!file.mFileOpen,"original close updates state");
            require(file.open(entry),"original fast-open by FST entry");
        }
        const u32 remaining=heap->getTotalFreeSize();
        if(!iteration) baseline=remaining;
        else require(remaining==baseline,"repeated file and stream lifetimes do not leak game-heap storage");
    }
    require(p2_dvd_mount(argv[1]),"all native DVD handles closed after JKR destruction");
    std::puts("Original JKRDvdFile/JSUFileInputStream: real title reads, callbacks, errors, seeks, concurrent reads and cleanup pass.");
}
