#include "Dolphin/gx.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>
static int failures;
static void check(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",what);++failures; } }
int main() {
    for(unsigned height: {240u,264u,480u,528u,576u}) {
        check(GXGetYScaleFactor(height,height)==1.0f,"equal-height display scale");
        check(GXGetNumXfbLines(height,1.0f)==height,"unscaled framebuffer height");
    }
    check(GXGetNumXfbLines(240,2.0f)==479,"scanline interval doubling");
    check(GXGetNumXfbLines(528,2.0f)==1024,"hardware line cap");
    // Independent integer register calculation across the usable scaling range.
    for(unsigned height=240;height<=576;height+=24) {
        for(unsigned target=height;target<=576;target+=16) {
            const float scale=GXGetYScaleFactor(height,target);
            const unsigned reg=unsigned(256.0f/scale)&511;
            unsigned expected=((height-1)*256)/reg+1;
            unsigned odd=reg; while(!(odd&1)) odd>>=1;
            if(reg>128&&reg<256&&height%odd==0) ++expected;
            if(expected>1024) expected=1024;
            check(GXGetNumXfbLines(height,scale)==expected&&expected<=target,"representable scale obeys target height");
        }
    }
    Mtx44 projection={};projection[0][0]=2;projection[3][3]=1;
    GXFogAdjTable table{};GXInitFogAdjTable(&table,640,projection);
    for(unsigned i=0;i<10;++i) {
        const double x=(i+1)*64.0/640.0;
        const unsigned expected=unsigned(std::sqrt(1+x*x/3)*256)&4095;
        check(table.fogVals[i]==expected,"orthographic fog distance");
    }
    projection[3][3]=0;projection[2][3]=-2;projection[2][2]=-1;
    GXInitFogAdjTable(&table,640,projection);
    for(unsigned i=0;i<10;++i) {
        const double x=(i+1)*32.0/640.0;
        const unsigned expected=unsigned(std::sqrt(1+x*x)*256)&4095;
        check(table.fogVals[i]==expected,"perspective fog distance");
    }
    std::printf("Display calculations: %s\n",failures?"FAILED":"passed");return failures?1:0;
}
