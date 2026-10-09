#include "p2_host_compat.h"
#include "Dolphin/dvd.h"
#include "p2_dvd.h"
#include <vector>

int main() {
    DVDInit();
    if (!DVDCheckDisk()) return 1;
    char path[] = "/user/Kando/piki/pikis.szs";
    const s32 entry = DVDConvertPathToEntrynum(path);
    DVDFileInfo info{};
    if (entry < 0 || !DVDFastOpen(entry, &info)) return 2;
    unsigned char magic[32];
    if (DVDReadPrio(&info, magic, 32, 0, 2) != 32 || memcmp(magic,"Yaz0",4)) return 3;
    if (DVDReadPrio(&info, magic, 32, -1, 2) != DVD_RESULT_FATAL_ERROR) return 4;
    if (DVDReadPrio(&info, magic, 32, info.length+32, 2) != DVD_RESULT_FATAL_ERROR) return 5;
    const auto size = info.length;
    if (!DVDClose(&info) || DVDReadPrio(&info,magic,32,0,2) != DVD_RESULT_FATAL_ERROR) return 6;
    char folder[] = "/user/Kando/piki";
    if (!DVDChangeDir(folder)) return 7;
    char relative[] = "pikis.szs";
    if (DVDConvertPathToEntrynum(relative) != entry || !DVDOpen(relative,&info)) return 8;
    DVDClose(&info);
    char root[] = "/"; DVDChangeDir(root);
    char escape[] = "../sys/main.dol";
    if (DVDConvertPathToEntrynum(escape) != -1) return 9;
    DVDDir directory{}; DVDDirEntry child{};
    if (!DVDOpenDir(folder,&directory)) return 10;
    bool found = false;
    while (DVDReadDir(&directory,&child)) if (child.entryNum == u32(entry)) found = true;
    DVDCloseDir(&directory);
    if (!found) return 11;
    printf("Native DVD checks passed: original FST entry %d, %u-byte Pikmin archive, relative paths and directory iteration\n", entry,size);
    return 0;
}
