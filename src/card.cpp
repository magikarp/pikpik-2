#define CARDInit p2_aurora_card_init
#include <dolphin/card.h>
#undef CARDInit
#include "p2_game_alloc.h"
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <cstddef>
// Disk/file records exchanged across the two SDK header sets have identical
// fixed-width layouts; no native pointers appear in these structures.
static_assert(sizeof(CARDFileInfo)==20 && offsetof(CARDFileInfo,iBlock)==16);
static_assert(sizeof(CARDStat)==108 && offsetof(CARDStat,iconAddr)==48 && offsetof(CARDStat,offsetData)==104);
extern "C" void CARDInit() {
    // Card images and their formatting buffer belong to the host service, not
    // whichever small JKR heap System::construct currently has selected.
    P2HostScratchScope hostStorage;
    static std::once_flag initialized;
    std::call_once(initialized,[] {
        const char* configured=std::getenv("P2_CARD_DIRECTORY");
        const std::filesystem::path directory=configured&&*configured?configured:P2_CARD_ROOT;
        std::error_code error;
        std::filesystem::create_directories(directory,error);
        if(error) {
            std::fprintf(stderr,"Pikmin 2 cannot create card directory: %s\n",error.message().c_str());
            std::abort();
        }
        CARDSetLoadType(CARD_RAWIMAGE);
        CARDSetBasePath(directory.c_str(),-1);
        p2_aurora_card_init("GPVE","01");
    });
}
