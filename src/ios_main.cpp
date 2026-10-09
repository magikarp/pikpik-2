// iOS entry point. The Xcode shell's main() (ios/Pikmin2/main.m) hands this to SDL_RunApp.
//
// Everything the app reads or writes lives in its Documents folder, which the
// Files app and Finder expose:
//   disc/                extracted GPVE01 disc (sys/ and files/); a folder named
//                        a folder named "extracted-disc" also works
//   textures/            optional Dolphin-format texture pack (GPV/ also works);
//                        used automatically when present
//   cards/               memory card
//   p2_settings.txt      KEY=VALUE lines, applied as environment variables: the
//                        same P2_* settings the desktop reads
//   bench_matrix.txt     optional: one bench run per launch (see below)
//   stderr.log           this launch's log (stderr.prev.log: the launch before)
// The working directory is Documents, so relative paths in settings resolve there.
//
// bench_matrix.txt: each non-comment line is "label KEY=VALUE KEY=VALUE ...".
// Every launch applies the next line, runs it and exits; bench_matrix.next holds
// the position. Delete bench_matrix.next to start over; after the last line the
// game starts normally. Lines labelled "all" apply to every run, and {label} in a
// value becomes the run's label.
//
// Defaults for normal play (not bench runs), each overridable in p2_settings.txt:
// the screen's own resolution, MSAA 4, and the texture pack if one is present
// (P2_TEXTURE_PACK= with no value turns it off). Measured on an M1 iPad Pro:
// all three together hold 120 Hz (docs/HANDOFF.md, "iPad build").
#include <SDL3/SDL_hints.h>
#include <os/proc.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

int p2_host_main(int argc,char** argv); // src/game_main.cpp, renamed from main on iOS

namespace {
namespace fs=std::filesystem;

std::string trim(const std::string& text) {
    const auto first=text.find_first_not_of(" \t\r\n");
    if(first==std::string::npos) return {};
    return text.substr(first,text.find_last_not_of(" \t\r\n")-first+1);
}
void applySetting(const std::string& setting,const char* source) {
    const auto equals=setting.find('=');
    if(equals==std::string::npos || equals==0) {
        std::fprintf(stderr,"[IOS] *** %s: ignoring '%s' (expected KEY=VALUE)\n",source,setting.c_str()); return;
    }
    const std::string key=trim(setting.substr(0,equals)), value=trim(setting.substr(equals+1));
    setenv(key.c_str(),value.c_str(),1);
    std::fprintf(stderr,"[IOS] %s: %s=%s\n",source,key.c_str(),value.c_str());
}
void applySettingsFile() {
    std::ifstream in("p2_settings.txt");
    for(std::string line;std::getline(in,line);) {
        line=trim(line);
        if(!line.empty() && line[0]!='#') applySetting(line,"p2_settings.txt");
    }
}
// Applies the next bench_matrix.txt line, if any are left.
bool applyBenchMatrix() {
    std::ifstream in("bench_matrix.txt");
    if(!in) return false;
    std::vector<std::string> runs, shared;
    for(std::string line;std::getline(in,line);) {
        line=trim(line);
        if(line.empty() || line[0]=='#') continue;
        if(line.rfind("all ",0)==0) shared.push_back(line.substr(4)); else runs.push_back(line);
    }
    size_t next=0;
    if(std::ifstream state{"bench_matrix.next"}) state>>next;
    if(next>=runs.size()) {
        std::fprintf(stderr,"[IOS] bench matrix: all %zu runs done (delete bench_matrix.next to repeat)\n",runs.size());
        return false;
    }
    std::ofstream("bench_matrix.next",std::ios::trunc)<<next+1<<"\n";
    std::istringstream words(runs[next]);
    std::string label; words>>label;
    std::fprintf(stderr,"[IOS] bench matrix: run %zu of %zu, %s\n",next+1,runs.size(),label.c_str());
    setenv("P2_BENCH_LABEL",label.c_str(),1);
    std::string settings;
    for(const auto& line:shared) settings+=line+" ";
    for(std::string word;words>>word;) settings+=word+" ";
    std::istringstream all(settings);
    for(std::string setting;all>>setting;) {
        for(size_t at;(at=setting.find("{label}"))!=std::string::npos;) setting.replace(at,7,label);
        applySetting(setting,"bench_matrix.txt");
    }
    return true;
}
// P2_CARD_SNAPSHOT=dir: replace the card directory's files with dir's before the
// game starts (what tools/bench.sh does with a recording's .save directory).
void restoreCardSnapshot() {
    const char* snapshot=std::getenv("P2_CARD_SNAPSHOT");
    const char* cards=std::getenv("P2_CARD_DIRECTORY");
    if(!snapshot || !*snapshot || !cards) return;
    std::error_code error;
    fs::create_directories(cards,error);
    for(const auto& entry:fs::directory_iterator(cards,error)) if(entry.is_regular_file()) fs::remove(entry.path(),error);
    for(const auto& entry:fs::directory_iterator(snapshot,error))
        if(entry.is_regular_file()) fs::copy_file(entry.path(),fs::path(cards)/entry.path().filename(),
            fs::copy_options::overwrite_existing,error);
    std::fprintf(stderr,"[IOS] card restored from %s%s\n",snapshot,error?" *** with errors":"");
}
}

extern "C" int p2_ios_main(int argc,char** argv) {
    // "playback" plays with the mute switch on; SDL's default ("ambient") is silenced by it.
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY,"playback");
    const char* home=std::getenv("HOME");
    const fs::path documents=fs::path(home?home:".")/"Documents";
    std::error_code error;
    fs::create_directories(documents/"cards",error);
    if(chdir(documents.c_str())!=0) std::fprintf(stderr,"[IOS] *** cannot enter %s\n",documents.c_str());
    // A home-screen launch has no console: keep this launch's stderr in Documents
    // (stderr.log) and the previous launch's in stderr.prev.log.
    fs::rename("stderr.log","stderr.prev.log",error);
    if(std::freopen("stderr.log","w",stderr)) setvbuf(stderr,nullptr,_IONBF,0);
    fs::path disc=documents/"disc";
    if(!fs::exists(disc/"sys") && fs::exists(documents/"extracted-disc"/"sys")) disc=documents/"extracted-disc";
    setenv("P2_DISC_DIRECTORY",disc.c_str(),1);
    setenv("P2_CARD_DIRECTORY",(documents/"cards").c_str(),1);
    applySettingsFile();
    if(!applyBenchMatrix()) {
        setenv("P2_MSAA","4",0);
        for(const char* pack:{"textures","GPV"})
            if(fs::is_directory(documents/pack)) { setenv("P2_TEXTURE_PACK",pack,0); break; }
    }
    restoreCardSnapshot();
    if(const char* dumps=std::getenv("P2_DUMP_DIR")) fs::create_directories(dumps,error);
    const double availableMB=os_proc_available_memory()/1048576.0;
    std::fprintf(stderr,"[IOS] documents %s, memory available to the app at launch: %.0f MB\n",documents.c_str(),availableMB);
    setenv("P2_LAUNCH_AVAILABLE_MB",std::to_string(int(availableMB)).c_str(),1); // reported in the bench summary
    return p2_host_main(argc,argv);
}
