#include "JSystem/JUtility/JUTException.h"
#include "Dolphin/os.h"
#include <cerrno>
#include <cstdio>
#include <string>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static bool checkPanic(bool jutility) {
    int output[2];if(pipe(output)) return false;
    const pid_t child=fork();
    if(child<0) { close(output[0]);close(output[1]);return false; }
    if(!child) {
        close(output[0]);dup2(output[1],STDERR_FILENO);close(output[1]);
        const rlimit noCore{0,0};setrlimit(RLIMIT_CORE,&noCore);
        if(jutility) JUTException::panic_f("native-test.cpp",73,"literal 100%% value=%d text=%s",42,"ready");
        else OSPanic("os-test.cpp",19,"literal 100%% value=%d text=%s",42,"ready");
        _exit(90); // Returning from a panic is a failure.
    }
    close(output[1]);
    std::string report;char buffer[1024];
    for(;;) {
        const auto size=read(output[0],buffer,sizeof(buffer));
        if(size>0) report.append(buffer,size);
        else if(size<0&&errno==EINTR) continue;
        else break;
    }
    close(output[0]);
    int status=0;pid_t waited;
    do { waited=waitpid(child,&status,0); } while(waited<0&&errno==EINTR);
    return waited==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT &&
        report.find(jutility?"native-test.cpp:73":"os-test.cpp:19")!=std::string::npos &&
        report.find("literal 100% value=42 text=ready")!=std::string::npos &&
        report.find("Native stack trace:\n")!=std::string::npos &&
        report.find("p2_panic_checks")!=std::string::npos;
}
int main() {
    if(!checkPanic(true)||!checkPanic(false)) {
        std::fputs("Native panic failed to report, preserve formatting, or terminate with SIGABRT\n",stderr);return 1;
    }
    std::puts("Original JUT panic and OSPanic report source, formatted message and native stack, then terminate.");
}
