#include "p2_thp_decode.h"
#include "p2_assets.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

static void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "THP check failed: %s\n", message); std::exit(1); }
}
static std::uint32_t be(const std::uint8_t* p) {
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];
}
// Put the final compressed byte directly against an inaccessible page. A
// truncated entropy stream must return an error, never read padded storage.
struct GuardedInput {
    void* mapping;
    std::size_t mappingSize;
    std::uint8_t* data;
    GuardedInput(const std::uint8_t* source, std::size_t size) {
        const auto page=std::size_t(sysconf(_SC_PAGESIZE));
        const auto usable=((size+page-1)/page)*page;
        mappingSize=usable+page;
        mapping=mmap(nullptr,mappingSize,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
        require(mapping!=MAP_FAILED,"input mapping");
        auto* end=static_cast<std::uint8_t*>(mapping)+usable;
        require(mprotect(end,page,PROT_NONE)==0,"input guard");
        data=end-size;
        if(size) std::memcpy(data,source,size);
    }
    ~GuardedInput() { munmap(mapping,mappingSize); }
};
int main(int argc,char** argv) {
    require(argc==2,"disc argument");
    unsigned movies=0,frames=0;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(std::filesystem::path(argv[1])/"files/thp")) {
        if(entry.path().extension()!=".thp") continue;
        const auto bytes=p2::readAsset(entry.path().parent_path(),entry.path().filename());
        require(bytes.size()>=48 && !std::memcmp(bytes.data(),"THP\0",4),"THP header");
        const auto components=be(bytes.data()+32);
        require(components+32<=bytes.size(),"component header bounds");
        const auto count=be(bytes.data()+components);
        require(count>=1 && count<=16 && bytes[components+4]==0,"video first component");
        const auto width=be(bytes.data()+components+20),height=be(bytes.data()+components+24);
        require(width && height && width<=1024 && height<=1024,"dimensions");
        const auto tiled=[](size_t w,size_t h) { return ((w+7)/8)*((h+3)/4)*32; };
        const auto ys=tiled(width,height),cs=tiled((width+1)/2,(height+1)/2);
        std::vector<std::uint8_t> y(ys+64,0xa5),u(cs+64,0xa5),v(cs+64,0xa5);
        size_t offset=be(bytes.data()+40),size=be(bytes.data()+24);
        const auto frameCount=be(bytes.data()+20);
        require(frameCount,"frame count");
        for(unsigned frame=0;frame<frameCount;++frame) {
            require(offset<=bytes.size() && size<=bytes.size()-offset && size>=8+4*count,"frame bounds");
            const auto* block=bytes.data()+offset;
            const auto videoSize=be(block+8);
            const auto* video=block+8+4*count;
            require(videoSize<=size-(8+4*count),"video payload bounds");
            if(frame==0 || frame==frameCount/2 || frame+1==frameCount) {
                GuardedInput input(video,videoSize);
                require(p2_thp_decode(input.data,videoSize,y.data(),ys,u.data(),cs,v.data(),cs,width,height)==0,"real video decode");
                for(const auto* output:{&y,&u,&v})
                    require(std::all_of(output->end()-64,output->end(),[](auto byte){return byte==0xa5;}),"output guard intact");
                require(p2_thp_decode(input.data,videoSize,y.data(),ys-1,u.data(),cs,v.data(),cs,width,height)!=0,"short output rejected");
                require(p2_thp_decode(input.data,videoSize,y.data(),ys,u.data(),cs,v.data(),cs,width+1,height)!=0,"mismatched dimensions rejected");
                if(frame==frameCount/2) {
                    // Exercise every early-header truncation, plus entropy
                    // truncation well into the frame, with a protected boundary.
                    for(size_t cut=0;cut<std::min<size_t>(videoSize,128);++cut) {
                        GuardedInput shortInput(video,cut);
                        require(p2_thp_decode(shortInput.data,cut,y.data(),ys,u.data(),cs,v.data(),cs,width,height)!=0,"truncated header rejected");
                    }
                    GuardedInput shortInput(video,videoSize/2);
                    require(p2_thp_decode(shortInput.data,videoSize/2,y.data(),ys,u.data(),cs,v.data(),cs,width,height)!=0,"truncated entropy rejected");
                }
                ++frames;
            }
            offset+=size; size=be(block);
        }
        ++movies;
        std::printf("%s: %ux%u, %u frames indexed\n",entry.path().filename().c_str(),width,height,frameCount);
    }
    require(movies==12 && frames==36,"all supplied movies sampled");
    std::printf("Decoded %u real frames from %u movies; bounded malformed-input checks passed.\n",frames,movies);
}
