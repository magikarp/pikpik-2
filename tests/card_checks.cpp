#include "Dolphin/card.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "p2_memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <unistd.h>
#include <sys/wait.h>
static_assert(sizeof(CARDFileInfo)==20 && offsetof(CARDFileInfo,iBlock)==16,"card file ABI");
static_assert(sizeof(CARDStat)==108 && offsetof(CARDStat,iconAddr)==48 && offsetof(CARDStat,offsetData)==104,"card status ABI");
static int failures;
static void check(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",what); ++failures; } }
int main(int argc,char** argv) {
    if(!p2_memory_init(16*1024*1024)) return 1;
    auto* root=JKRExpHeap::createRoot(16,false);
    auto* gameHeap=root?JKRExpHeap::create(256*1024,root,false):nullptr;
    if(!gameHeap) return 1;
    gameHeap->becomeCurrentHeap();
    if(argc>1) {
        CARDInit();
        if(!std::strcmp(argv[1],"corrupt")) {
            check(CARDMount(0,nullptr,nullptr)==CARD_RESULT_IOERROR,"damaged card rejected without formatting");
            return failures?1:0;
        }
        check(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY,"fresh-process mount");
        CARDFileInfo file{};
        if(CARDOpen(0,const_cast<char*>("Pikmin2-native-test"),&file)!=CARD_RESULT_READY) return 1;
        unsigned char bytes[8192];
        check(CARDRead(&file,bytes,sizeof(bytes),0)==CARD_RESULT_READY,"fresh-process read");
        for(unsigned i=0;i<sizeof(bytes);++i) if(bytes[i]!=static_cast<unsigned char>(i*17+3)) { check(false,"persistent disc bytes");break; }
        CARDStat status{};
        check(CARDGetStatus(0,file.fileNo,&status)==CARD_RESULT_READY&&status.commentAddr==0&&status.iconAddr==64,"persistent metadata");
        CARDClose(&file); CARDUnmount(0); return failures?1:0;
    }
    char directory[]="/tmp/pikmin2-card-test-XXXXXX";
    if(!mkdtemp(directory)) return 1;
    setenv("P2_CARD_DIRECTORY",directory,1);
    const auto freeBeforeInit=gameHeap->getTotalFreeSize();
    CARDInit(); CARDInit();
    check(gameHeap->getTotalFreeSize()==freeBeforeInit,"host card initialization leaves selected small game heap untouched");
    check(CARDProbe(0),"new slot A card is present");
    check(!CARDProbe(1),"absent slot B remains absent");
    s32 size=0,sector=0;
    check(CARDProbeEx(0,&size,&sector)==CARD_RESULT_READY&&sector==8192,"probe card geometry");
    check(CARDMount(0,nullptr,nullptr)==CARD_RESULT_READY,"mount new card");
    check(CARDCheck(0)==CARD_RESULT_READY,"card consistency");
    CARDFileInfo file{};
    const s32 created=CARDCreate(0,const_cast<char*>("Pikmin2-native-test"),8192,&file);
    check(created==CARD_RESULT_READY,"create save file");
    if(created==CARD_RESULT_READY) {
        alignas(32) unsigned char data[8192],readback[8192];
        for(unsigned i=0;i<sizeof(data);++i) data[i]=static_cast<unsigned char>(i*17+3);
        check(CARDWrite(&file,data,sizeof(data),0)==CARD_RESULT_READY,"write save bytes");
        check(CARDRead(&file,readback,sizeof(readback),0)==CARD_RESULT_READY&&!std::memcmp(data,readback,sizeof(data)),"read exact save bytes");
        CARDStat status{};
        check(CARDGetStatus(0,file.fileNo,&status)==CARD_RESULT_READY,"read metadata");
        check(status.length==8192&&!std::memcmp(status.gameName,"GPVE",4)&&!std::memcmp(status.company,"01",2),"game and maker metadata ABI");
        status.commentAddr=0;status.iconAddr=64;status.bannerFormat=0;status.iconFormat=0;status.iconSpeed=0;
        check(CARDSetStatus(0,file.fileNo,&status)==CARD_RESULT_READY,"write metadata");
        check(CARDClose(&file)==CARD_RESULT_READY,"close save");
        check(CARDOpen(0,const_cast<char*>("Pikmin2-native-test"),&file)==CARD_RESULT_READY,"reopen save");
        check(CARDRead(&file,readback,sizeof(readback),0)==CARD_RESULT_READY&&!std::memcmp(data,readback,sizeof(data)),"reopened save bytes persist");
        CARDClose(&file);
    }
    check(CARDUnmount(0)==CARD_RESULT_READY,"unmount closes backing file");
    check(CARDCheck(0)==CARD_RESULT_NOCARD,"unmounted card is not ready");
    check(CARDProbe(0),"unmount does not remove physical card");
    check(CARDMount(1,nullptr,nullptr)==CARD_RESULT_NOCARD,"mount absent card rejects");
    const pid_t child=fork();
    if(child==0) { execl(argv[0],argv[0],"verify",nullptr); _exit(127); }
    int status=0;
    check(child>0&&waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh-process persistence");
    char image[512];std::snprintf(image,sizeof(image),"%s/MemoryCardA.USA.raw",directory);
    unlink(image);
    FILE* damaged=std::fopen(image,"wb");
    check(damaged!=nullptr,"create isolated damaged card");
    if(damaged) { std::fwrite("BAD!",1,4,damaged); std::fclose(damaged); }
    const pid_t corruptChild=fork();
    if(corruptChild==0) { execl(argv[0],argv[0],"corrupt",nullptr); _exit(127); }
    check(corruptChild>0&&waitpid(corruptChild,&status,0)==corruptChild&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"damaged-card initialization");
    damaged=std::fopen(image,"rb");
    if(damaged) {
        char bytes[8]={}; const auto count=std::fread(bytes,1,sizeof(bytes),damaged);std::fclose(damaged);
        check(count==4&&!std::memcmp(bytes,"BAD!",4),"existing damaged card was not overwritten");
    } else check(false,"existing damaged card remains present");
    unlink(image);rmdir(directory);
    std::printf("Native card checks: %s (isolated files: %s)\n",failures?"FAILED":"passed",directory);
    return failures?1:0;
}
