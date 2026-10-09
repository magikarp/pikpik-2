#include "JSystem/JParticle/JPABlock.h"
#include "JSystem/JParticle/JPAShape.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_particle.h"
#include "p2_dvd.h"
#include "JSystem/JKernel/JKRDvdRipper.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void require(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); } }
static u16 word(const u8* p) { return (unsigned(p[0])<<8)|p[1]; }
static u32 dword(const u8* p) { return (u32(word(p))<<16)|word(p+2); }
static float scalar(const u8* p) { u32 bits=dword(p); float f; std::memcpy(&f,&bits,4); return f; }
void makeColorTable(GXColor**,const JPAClrAnmKeyData*,u8,s16,JKRHeap*);
static float referenceKey(const u8* p,float frame) {
    unsigned n=p[9]; const auto* keys=p+12;
    if(p[11]) { int period=int(scalar(keys+16*(n-1)))+1; frame-=int(frame)/period*period; }
    if(frame<scalar(keys)) return scalar(keys+4);
    if(frame>=scalar(keys+16*(n-1))) return scalar(keys+16*(n-1)+4);
    unsigned i=0; while(i+1<n && frame>=scalar(keys+16*(i+1))) ++i;
    const auto* k=keys+16*i; double duration=scalar(k+16)-scalar(k),t=(frame-scalar(k))/duration;
    return (2*t*t*t-3*t*t+1)*scalar(k+4)+(t*t*t-2*t*t+t)*duration*scalar(k+12)+
        (-2*t*t*t+3*t*t)*scalar(k+20)+(t*t*t-t*t)*duration*scalar(k+24);
}
int main(int argc,const char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024),"arena and disc");
    auto* heap=JKRExpHeap::createRoot(16,false); require(heap,"game heap");
    require(p2_dvd_mount(argv[1]),"particle DVD mount");
    Mtx rotation; Vec axis={0,0,2},point={1,0,0};
    PSMTXRotAxisRad(rotation,&axis,1.57079632679f);
    rotation[0][3]=123; rotation[1][3]=456;
    PSMTXMultVecSR(rotation,&point,&point);
    require(std::fabs(point.x)<0.00001f && std::fabs(point.y-1)<0.00001f && point.z==0,
        "native axis rotation normalizes input and in-place SR excludes translation");
    JPAClrAnmKeyData colorKeys[3]={{1,{10,20,30,40}},{3,{50,60,70,80}},{4,{255,0,0,0}}};
    GXColor* table=nullptr;
    makeColorTable(&table,colorKeys,2,5,heap);
    require(table && table[0].r==10 && table[2].r==30 && table[3].r==50 && table[4].r==50 && table[5].r==50,
        "color animation holds endpoints and never consumes a key beyond the declared count");
    delete[] table;
    unsigned resources=0,blocks=0,samples=0;
    for(const char* path:{"user/Ebisawa/effect/game.jpc","user/Ebisawa/effect/eff2d_game2d.jpc","user/Ebisawa/effect/eff2d_file_select.jpc"}) {
        auto bytes=p2::readAsset(std::filesystem::path(argv[1])/"files",path);
        const char* validationError=nullptr;
        if(!p2_validate_jpc(bytes.data(),bytes.size(),&validationError)) {
            std::fprintf(stderr,"%s: %s\n",path,validationError); return 1;
        }
        u32 loadedSize=0;
        void* loaded=JKRDvdRipper::loadToMainRAM(path,nullptr,Switch_0,0,heap,JKRDvdRipper::ALLOC_DIR_TOP,0,nullptr,&loadedSize);
        require(loaded && loadedSize>=bytes.size() && p2_validate_jpc(loaded,loadedSize),"actual DVD loader supplies a valid bounded particle resource");
        heap->free(loaded);
        require(!p2_validate_jpc(nullptr,bytes.size()) && !p2_validate_jpc(bytes.data(),15),"reject missing or truncated JPC header");
        auto broken=bytes;
        auto put32=[&](size_t offset,u32 value) {
            for(unsigned i=0;i<4;++i) broken[offset+i]=u8(value>>(24-8*i));
        };
        put32(12,UINT32_MAX);
        require(!p2_validate_jpc(broken.data(),broken.size()),"reject out-of-range texture section");
        broken=bytes; put32(28,0);
        require(!p2_validate_jpc(broken.data(),broken.size()),"reject zero-length first particle block");
        broken=bytes; ++broken[20];
        require(!p2_validate_jpc(broken.data(),broken.size()),"reject mismatched field count");
        const size_t firstTexture=dword(bytes.data()+12);
        broken=bytes; put32(firstTexture+32+28,UINT32_MAX);
        require(!p2_validate_jpc(broken.data(),broken.size()),"reject texture image outside TEX1 payload");
        require(bytes.size()>=16 && !std::memcmp(bytes.data(),"JPAC2-10",8),"particle archive version");
        const auto textureStart=dword(bytes.data()+12); size_t at=16;
        require(textureStart<=bytes.size(),"texture boundary");
        for(unsigned r=0;r<word(bytes.data()+8);++r) {
            require(at+8<=textureStart,"resource header extent");
            unsigned count=word(bytes.data()+at+2); at+=8; ++resources;
            for(unsigned b=0;b<count;++b) {
                require(at+8<=textureStart,"block header extent");
                const auto* p=bytes.data()+at; const unsigned size=dword(p+4);
                require(size>=8 && size<=textureStart-at,"block extent");
                const auto freeBefore=heap->getTotalFreeSize();
                if(!std::memcmp(p,"BEM1",4)) {
                    require(size>=0x7c,"emitter extent"); JPADynamicsBlock emitter(p);
                    require(emitter.getFlag()==dword(p+8) && emitter.getRate()==scalar(p+0x4c) &&
                        emitter.getLifetime()==s16(word(p+0x72)) && emitter.getVolumeSize()==word(p+0x74),"original emitter parameters");
                    JGeometry::TVec3f scale; emitter.getEmitterScl(&scale);
                    require(scale.x==scalar(p+16) && scale.y==scalar(p+20) && scale.z==scalar(p+24),"emitter vector components");
                } else if(!std::memcmp(p,"KFA1",4)) {
                    broken=bytes; broken[at+9]=0;
                    require(!p2_validate_jpc(broken.data(),broken.size()),"reject empty particle key table");
                    require(size>=12 && p[9] && 12+unsigned(p[9])*16<=size,"keyframe extent");
                    for(unsigned k=0;k<p[9];++k) require(std::isfinite(scalar(p+12+16*k)),"finite key time");
                    JPAKeyBlock key(p);
                    for(unsigned k=0;k<p[9];++k) for(float delta:{-0.25f,0.0f,0.25f}) {
                        const float frame=scalar(p+12+16*k)+delta,expected=referenceKey(p,frame),actual=key.calc(frame);
                        require(std::fabs(actual-expected)<=0.0001f*(1+std::fabs(expected)),"original particle keyframe interpolation"); ++samples;
                    }
                } else if(!std::memcmp(p,"BSP1",4)) {
                    require(size>=sizeof(JPABaseShapeData),"base-shape extent");
                    for(int c=0;c<2;++c) if(p[0x21]&(c ? 8 : 2)) {
                        const unsigned offset=word(p+12+c*2),count=p[34+c];
                        require(count && offset<=size && 6*count<=size-offset,"color-key table extent");
                        for(unsigned k=1;k<count;++k) require(word(p+offset+6*k)>word(p+offset+6*(k-1)),"ordered color keys");
                        broken=bytes; broken[at+12+2*c]=255; broken[at+13+2*c]=255;
                        require(!p2_validate_jpc(broken.data(),broken.size()),"reject color table outside its block");
                    }
                    if((dword(p+8)&0x1000000)!=0) require(sizeof(JPABaseShapeData)+40<=size,"texture animation extent");
                    JPABaseShape shape(p,heap);
                    require(float(shape.mData->mBaseSizeX)==scalar(p+16) && float(shape.mData->mBaseSizeY)==scalar(p+20),"base shape sizes");
                    if(shape.isTexCrdAnm()) require(shape.getInitTransX()==scalar(p+sizeof(JPABaseShapeData)) &&
                        shape.getIncRot()==scalar(p+sizeof(JPABaseShapeData)+36),"texture-coordinate animation floats");
                    for(int c=0;c<2;++c) if(p[0x21]&(c ? 8 : 2)) {
                        const u8* keys=p+word(p+12+c*2); const unsigned count=p[34+c];
                        const auto* colors=reinterpret_cast<const u8*>(c ? shape.mEnvClrAnmTbl : shape.mPrmClrAnmTbl);
                        for(int frame=0;frame<=s16(word(p+36));++frame) {
                            unsigned k=0; while(k+1<count && word(keys+6*(k+1))<=frame) ++k;
                            for(int channel=0;channel<4;++channel) {
                                double expected=keys[k*6+2+channel];
                                if(k+1<count && frame>=word(keys+k*6)) expected+=(keys[(k+1)*6+2+channel]-expected)*
                                    (frame-word(keys+k*6))/double(word(keys+(k+1)*6)-word(keys+k*6));
                                require(std::abs(int(colors[4*frame+channel])-int(expected))<=1,"original animated color table");
                            }
                        }
                    }
                    delete[] shape.mPrmClrAnmTbl; delete[] shape.mEnvClrAnmTbl;
                } else if(!std::memcmp(p,"FLD1",4)) {
                    require(size>=sizeof(JPAFieldBlock::Data),"field extent"); JPAFieldBlock field(p,heap);
                    field.initOpParam();
                    require(field.getMag()==scalar(p+36) && field.getPos().x==scalar(p+12) &&
                        field.getDir().z==scalar(p+32) && field.getType()==(dword(p+8)&15),"original force field parameters");
                    delete field.mField;
                } else if(!std::memcmp(p,"SSP1",4)) {
                    require(size>=sizeof(JPAChildShapeData),"child-shape extent"); JPAChildShape shape(p);
                    require(shape.getGravity()==scalar(p+28) && shape.getLife()==s16(word(p+64)),"child particle parameters");
                } else if(!std::memcmp(p,"ESP1",4)) {
                    require(size>=sizeof(JPAExtraShapeData),"extra-shape extent"); JPAExtraShape shape(p);
                    require(shape.getScaleInTiming()==scalar(p+12) && shape.getAlphaFreq()==scalar(p+64),"extra shape parameters");
                } else if(!std::memcmp(p,"ETX1",4)) {
                    require(size>=sizeof(JPAExTexShapeData),"indirect-texture extent"); JPAExTexShape shape(p);
                    for(int i=0;i<6;++i) require(shape.getIndTexMtx()[i]==scalar(p+12+4*i),"native indirect texture matrix");
                }
                require(heap->getTotalFreeSize()==freeBefore,"particle block metadata cleanup");
                at+=size; ++blocks;
            }
        }
    }
    for(const char* path:{"user/Ebisawa/effect/eff2d_world_map.jpc"}) {
        auto bytes=p2::readAsset(std::filesystem::path(argv[1])/"files",path);
        const char* error=nullptr;
        if(!p2_validate_jpc(bytes.data(),bytes.size(),&error)) { std::fprintf(stderr,"%s: %s\n",path,error); return 1; }
    }
    auto legacy=p2::readAsset(std::filesystem::path(argv[1])/"files","user/Kando/effect/game.jpc");
    require(!p2_validate_jpc(legacy.data(),legacy.size()),"reject the legacy JPAC1-00 file unsupported by this loader");
    require(resources && samples,"actual particle resources exercised");
    std::printf("Particle resources: %u resources, %u blocks, %u original keyframe samples; emitter/shape parameters and matrices pass. Rendering untested.\n",resources,blocks,samples);
}
