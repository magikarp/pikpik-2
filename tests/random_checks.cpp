#include "Dolphin/rand.h"
#include "p2_random.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
static void require(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"RNG: %s\n",what); std::exit(1); } }
int main() {
    p2_game_srand(1);
    for(int expected:{16838,5758,10113,17515,31051}) require(p2_game_rand()==expected,"original MSL sequence");
    for(uint32_t seed:{0u,1u,0x80000000u,0xffffffffu}) {
        p2_game_srand(seed);
        uint32_t reference=seed;
        for(unsigned i=0;i<100000;++i) {
            reference=reference*1103515245u+12345u;
            require(p2_game_rand()==int((reference>>16)&0x7fff),"seed/overflow sequence");
            reference=reference*1103515245u+12345u;
            const float value=randFloat();
            require(value==float((reference>>16)&0x7fff)/32768.0f && value>=0 && value<1,"original randFloat range and value");
            const int index=int(120*value);
            require(index>=0 && index<120,"boot shuffle index bounds");
        }
    }
    p2_game_srand(1); const int first=p2_game_rand();
    std::srand(123); for(unsigned i=0;i<10;++i) (void)std::rand();
    require(first==16838 && p2_game_rand()==5758,"host libc RNG independent");
    std::puts("MSL random sequence, original helper bounds and host isolation passed.");
}
