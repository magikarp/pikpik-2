#include "Game/MoviePlayer.h"
#include "Game/gameStages.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
int main(int argc, const char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    require(argc==2 && p2_memory_init(128*1024*1024),"native memory");
    auto* root=JKRExpHeap::createRoot(16,false);
    require(root && p2_dvd_mount(argv[1]),"heap and disc");
    char text[]="{v0.5} ";
    RamStream stream(text,sizeof(text)); stream.setMode(STREAM_MODE_TEXT,1);
    ID32 version; version.read(stream);
    std::printf("Parsed movie version: %08x (expected %08x)\n",version.getID(),u32('v0.5'));
    require(version.getID()=='v0.5',"movie version numeric value");
    require(!std::strcmp(version.getStr(), "v0.5"), "movie version text view");
    ID32 code('ABCD');
    require(!std::strcmp(code.getStr(), "ABCD"), "numeric ID string order");
    std::memcpy(code.getStr(), "WXYZ", 4); code.updateID();
    require(code.getID()=='WXYZ', "string-to-numeric ID conversion");
    code='ABCD';
    require(code.match('A*CD','*') && !code.match('A*CE','*'), "ID wildcard match");
    char written[32] = {};
    RamStream textOutput(written,sizeof(written)); textOutput.setMode(STREAM_MODE_TEXT,1);
    code.write(textOutput);
    require(!std::strcmp(written,"{ABCD} "), "ID text serialization");
    unsigned char binary[8] = {};
    RamStream output(binary,sizeof(binary)); code.write(output);
    require(!std::memcmp(binary,"DCBA",4), "ID binary wire order preserved");
    RamStream input(binary,4); ID32 roundtrip; roundtrip.read(input);
    require(roundtrip.getID()=='ABCD' && !std::strcmp(roundtrip.getStr(),"ABCD"), "binary ID roundtrip");
    Game::stageList=new Game::Stages;
    require(Game::stageList->getCourseCount()==5,"five supplied stages including test_map");
    for (int i=0;i<5;++i) {
        auto* course=Game::stageList->getCourseInfo(i);
        require(course && Game::stageList->getCourseInfo(const_cast<char*>(course->mName))==course,"stage name lookup");
        std::printf("Stage %d: %s\n",i,course->mName);
    }
    Game::MovieList movies;
    unsigned count=0, mapped=0;
    for(auto* node=movies.mConfig.mChild;node;node=node->mNext) {
        auto* movie=static_cast<Game::MovieConfig*>(node);
        require(movie->mMovieNameBuffer1[0] && movie->mMovieNameBuffer2[0],"movie names parsed");
        if(std::strcmp(movie->mMapName,"nomap")) {
            auto* course=Game::stageList->getCourseInfo(movie->mMapName);
            require(course && movie->mCourseIndex==course->mCourseIndex,"movie map resolves to original stage"); ++mapped;
        } else require(movie->mCourseIndex==-1,"unmapped movie");
        ++count;
    }
    require(count==168 && mapped>0,"all movie configurations loaded");
    std::printf("Original movie loader: %u configurations, %u stage mappings pass.\n",count,mapped);
}
