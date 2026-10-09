#include "p2_reset.h"
#include "System.h"
#include "Dolphin/os.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static void shutdownProbe() { std::puts("shutdown-before-reset");std::fflush(stdout); }
static int childMain(int argc,char** argv) {
    if(argc!=3||std::strcmp(argv[2],"argument with spaces %")||!p2_reset_initialize(argc,argv,shutdownProbe)) return 11;
    const bool warm=std::strcmp(argv[1],"warm")==0;
    if(std::getenv("P2_RESET_TEST_REENTERED")) {
        if(std::getenv("P2_NATIVE_RESET_VIDEO")) return 12;
        if(warm) {
            if(RENDER_INFO_STORE->mIdentifier!=0x76616c64u||RENDER_INFO_STORE->mRenderMode!=1) return 13;
        } else if(RENDER_INFO_STORE->mIdentifier||RENDER_INFO_STORE->mRenderMode) return 14;
        std::puts("restarted-with-original-arguments");return 0;
    }
    if(RENDER_INFO_STORE->mIdentifier||RENDER_INFO_STORE->mRenderMode) return 15;
    RENDER_INFO_STORE->mIdentifier=0x76616c64u;RENDER_INFO_STORE->mRenderMode=1;
    OSSetSaveRegion(RENDER_INFO_STORE,reinterpret_cast<unsigned char*>(RENDER_INFO_STORE)+sizeof(RenderModeInfo));
    void* first=nullptr;void* last=nullptr;OSGetSaveRegion(&first,&last);
    if(first!=RENDER_INFO_STORE||last!=reinterpret_cast<unsigned char*>(first)+8) return 16;
    OSSetSaveRegion(nullptr,nullptr);OSGetSaveRegion(&first,&last);
    if(first||last) return 17;
    OSSetSaveRegion(RENDER_INFO_STORE,reinterpret_cast<unsigned char*>(RENDER_INFO_STORE)+sizeof(RenderModeInfo));
    setenv("P2_RESET_TEST_REENTERED","1",1);
    if(std::strcmp(argv[1],"menu")==0) OSResetSystem(OS_RESET_HOTRESET,0,TRUE);
    else if(std::strcmp(argv[1],"shutdown")==0) OSResetSystem(OS_RESET_SHUTDOWN,0,FALSE);
    else OSResetSystem(warm?OS_RESET_RESTART:OS_RESET_HOTRESET,0,FALSE);
    return 18;
}
static bool run(const char* path,const char* mode) {
    int channel[2];if(pipe(channel)) return false;
    const pid_t child=fork();
    if(child<0) return false;
    if(!child) {
        close(channel[0]);dup2(channel[1],STDOUT_FILENO);close(channel[1]);
        unsetenv("P2_RESET_TEST_REENTERED");unsetenv("P2_NATIVE_RESET_VIDEO");
        execl(path,path,mode,"argument with spaces %",static_cast<char*>(nullptr));_exit(19);
    }
    close(channel[1]);std::string output;char buffer[256];
    for(;;) {
        const auto count=read(channel[0],buffer,sizeof(buffer));
        if(count>0) output.append(buffer,count);
        else if(count<0&&errno==EINTR) continue;
        else break;
    }
    close(channel[0]);int status=0;pid_t waited;
    do { waited=waitpid(child,&status,0); } while(waited<0&&errno==EINTR);
    const bool restarted=std::strcmp(mode,"warm")==0||std::strcmp(mode,"cold")==0;
    const std::string expected=std::string("shutdown-before-reset\n")+(restarted?"restarted-with-original-arguments\n":"");
    if(waited!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=0||output!=expected) {
        std::fprintf(stderr,"Reset case %s failed: status=%d output=%s\n",mode,status,output.c_str());return false;
    }
    return true;
}
int main(int argc,char** argv) {
    if(argc>1) return childMain(argc,argv);
    for(const char* mode:{"warm","cold","menu","shutdown"}) if(!run(argv[0],mode)) return 1;
    std::puts("Native reset: soft video-state handoff, cold relaunch, menu/shutdown exit and cleanup order passed.");
}
