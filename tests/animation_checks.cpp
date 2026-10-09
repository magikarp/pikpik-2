#include "p2_animation.h"
#include "JSystem/J2D/J2DAnm.h"
#include "JSystem/J2D/J2DAnmLoader.h"
#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include "JSystem/J3D/J3DAnmTransform.h"
#include "JSystem/J3D/J3DAnmTevRegKey.h"
#include "JSystem/J3D/J3DAnmTextureSRTKey.h"
#include "JSystem/J3D/J3DTransform.h"
#include "JSystem/JKernel/JKRArchive.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
template<class T> static void store(unsigned char* dest, T value) {
    value=p2_big_endian(value); std::memcpy(dest,&value,sizeof(value));
}
static void knownCurve() {
    unsigned char block[152] = {};
    store<u32>(block,0x414e4b31); store<u32>(block+4,sizeof(block));
    block[9]=2; store<u16>(block+10,10); store<u16>(block+12,1);
    for(int i=14;i<=18;i+=2) store<u16>(block+i,6);
    store<u32>(block+20,36); store<u32>(block+24,92);
    store<u32>(block+28,116); store<u32>(block+32,128);
    for(int axis=0;axis<3;++axis) for(int channel=0;channel<3;++channel)
        store<u16>(block+36+axis*18+channel*6,2);
    const float scales[]={0,1,0,10,3,0}, positions[]={0,-10,0,10,10,0};
    const s16 rotations[]={0,0,0,10,100,0};
    for(int i=0;i<6;++i) {
        store<float>(block+92+i*4,scales[i]); store<s16>(block+116+i*2,rotations[i]);
        store<float>(block+128+i*4,positions[i]);
    }
    J3DAnmTransformKey animation;
    require(p2_load_transform(&animation,block,sizeof(block),true),"known key curve loads");
    animation.setFrame(2.5f);
    J3DTransformInfo info{}; animation.getTransform(0,&info);
    require(std::fabs(info.mScale.x-1.3125f)<1e-6f && std::fabs(info.mTranslation.z+6.875f)<1e-6f &&
            info.mRotation.y==60,"original evaluator matches analytic quarter-frame curve and rotation scaling");
    animation.setFrame(-1); animation.getTransform(0,&info);
    require(info.mScale.x==1 && info.mTranslation.y==-10 && info.mRotation.z==0,"key curve clamps before first sample");
    animation.setFrame(11); animation.getTransform(0,&info);
    require(info.mScale.x==3 && info.mTranslation.y==10 && info.mRotation.z==400,"key curve clamps after final sample");
    auto* storage=animation.mNativeStorage;
    store<u16>(block+38,0xffff);
    require(!p2_load_transform(&animation,block,sizeof(block),true) && animation.mNativeStorage==storage,
            "out-of-range channel rejected before replacing live data");
    unsigned char split[124] = {};
    store<u32>(split,0x414e4b31); store<u32>(split+4,sizeof(split));
    store<u16>(split+10,10); store<u16>(split+12,1); store<u16>(split+14,8);
    store<u32>(split+20,36); store<u32>(split+24,92);
    store<u32>(split+28,124); store<u32>(split+32,124);
    for(int axis=0;axis<3;++axis) {
        store<u16>(split+36+axis*18,2); store<u16>(split+40+axis*18,1);
    }
    const float splitValues[]={0,1,0,1,10,3,-1,0};
    for(int i=0;i<8;++i) store<float>(split+92+i*4,splitValues[i]);
    require(p2_load_transform(&animation,split,sizeof(split),true),"separate incoming/outgoing tangent curve loads");
    animation.setFrame(5); animation.getTransform(0,&info);
    require(std::fabs(info.mScale.y-4.5f)<1e-6f && info.mTranslation.x==0 && info.mRotation.z==0,
            "separate tangents and absent channels evaluate correctly");
}
template<class T> static T disk(const unsigned char* p) { T v; std::memcpy(&v,p,sizeof(v)); return p2_big_endian(v); }
static size_t checkTev(const unsigned char* block, size_t size) {
    J3DAnmTevRegKey animation;
    require(p2_load_tev_animation(&animation,block,size),"TRK1 color animation loads");
    size_t samples=0;
    for (int group=0;group<2;++group) {
        const size_t n=disk<u16>(block+12+group*2), tableAt=disk<u32>(block+32+group*4);
        auto& names=group ? animation.mKRegNameTable : animation.mCRegNameTable;
        for (size_t i=0;i<n;++i) {
            require(names.getName(i) && names.getIndex(names.getName(i))>=0,"owned material name lookup");
            for (float frame : {-1.0f,0.0f,0.5f,2.5f,5.0f,float(animation.getTotalFrameCount())+1}) {
                animation.setFrame(frame);
                GXColorS10 c{}; GXColor k{};
                if(group) animation.getTevKonstReg(i,&k); else animation.getTevColorReg(i,&c);
                const int actual[]={group ? k.r:c.r,group ? k.g:c.g,group ? k.b:c.b,group ? k.a:c.a};
                for (int channel=0;channel<4;++channel) {
                    const auto* track=block+tableAt+i*28+channel*6;
                    const size_t count=disk<u16>(track), first=disk<u16>(track+2), stride=disk<u16>(track+4) ? 4:3;
                    const auto* values=block+disk<u32>(block+56+(group*4+channel)*4)+first*2;
                    double expected=0;
                    auto value=[values](size_t at) { return double(disk<s16>(values+at*2)); };
                    if(count==1) expected=value(0);
                    else if(count>1) {
                        if(frame<=value(0)) expected=value(1);
                        else if(frame>=value((count-1)*stride)) expected=value((count-1)*stride+1);
                        else {
                            size_t key=0; while(frame>=value((key+1)*stride)) ++key;
                            const size_t a=key*stride,b=(key+1)*stride;
                            const double duration=value(b)-value(a), t=(frame-value(a))/duration;
                            expected=(2*t*t*t-3*t*t+1)*value(a+1)+(t*t*t-2*t*t+t)*duration*value(a+stride-1)
                                +(-2*t*t*t+3*t*t)*value(b+1)+(t*t*t-t*t)*duration*value(b+2);
                        }
                        expected=std::fmax(group ? 0:-1024,std::fmin(group ? 255:1023,expected));
                    }
                    const int quantized=group ? int(static_cast<u8>(static_cast<int>(expected))) : int(expected);
                    require(std::abs(actual[channel]-quantized)<=1,"original TEV sampler matches independent disk-key evaluation");
                }
                ++samples;
            }
        }
    }
    auto* storage=animation.mNativeStorage;
    require(!p2_load_tev_animation(&animation,block,12) && storage==animation.mNativeStorage,"bad TRK1 preserves live data");
    require(p2_load_tev_animation(&animation,block,size),"TRK1 replacement");
    return samples;
}
static void knownTevCurve() {
    unsigned char block[192]={};
    store<u32>(block,0x54524b31); store<u32>(block+4,sizeof(block)); store<u16>(block+10,10);
    for(int group=0;group<2;++group) {
        store<u16>(block+12+group*2,1); store<u32>(block+32+group*4,88+group*28);
        store<u32>(block+40+group*4,144+group*2); store<u32>(block+48+group*4,148+group*12);
        const int name=148+group*12;
        store<u16>(block+name,1); store<u16>(block+name+4,97); store<u16>(block+name+6,8); block[name+8]='a';
        for(int channel=0;channel<4;++channel) {
            store<u16>(block+16+(group*4+channel)*2,6);
            store<u32>(block+56+(group*4+channel)*4,176);
            store<u16>(block+88+group*28+channel*6,2);
        }
    }
    const s16 keys[]={0,-100,0,10,300,0};
    for(int i=0;i<6;++i) store<s16>(block+176+i*2,keys[i]);
    require(checkTev(block,sizeof(block))==12,"signed/unsigned color curves and clamp coverage");
    J3DAnmTevRegKey animation;
    store<u16>(block+144,0x1234);
    require(p2_load_tev_animation(&animation,block,sizeof(block)),"known TRK1");
    require(animation.getCRegUpdateMaterialID(0)==0x1234,"material IDs decode to native words");
    animation.mCRegUpdateMaterialID[0]=7;
    require(disk<u16>(block+144)==0x1234,"material remapping leaves archive bytes intact");
    block[156]='z';
    require(!std::strcmp(animation.mCRegNameTable.getName(0),"a"),"material names have independent ownership");
    block[156]='a';
    auto* storage=animation.mNativeStorage;
    store<u16>(block+90,0xffff);
    require(!p2_load_tev_animation(&animation,block,sizeof(block)) && animation.mNativeStorage==storage,"TRK1 channel range rejection");
    store<u16>(block+90,0); store<s16>(block+182,0);
    require(!p2_load_tev_animation(&animation,block,sizeof(block)),"TRK1 duplicate time rejection");
    store<s16>(block+182,10); store<u16>(block+154,0xffff);
    require(!p2_load_tev_animation(&animation,block,sizeof(block)),"TRK1 name range rejection");
    store<u16>(block+154,8);
    const s16 splitKeys[]={0,-100,0,10,10,300,-10,0};
    for(int i=0;i<8;++i) store<s16>(block+176+i*2,splitKeys[i]);
    for(int group=0;group<2;++group) for(int channel=0;channel<4;++channel) {
        store<u16>(block+16+(group*4+channel)*2,8);
        store<u16>(block+88+group*28+channel*6+4,1);
    }
    checkTev(block,sizeof(block)); // distinct incoming/outgoing tangents
    store<u16>(block+88,0); store<u16>(block+116,1);
    checkTev(block,sizeof(block)); // absent and constant channels
}
template<class T> static double sampleDiskKey(const unsigned char* track,const unsigned char* values,float frame,double fallback) {
    const size_t n=disk<u16>(track), first=disk<u16>(track+2), stride=disk<u16>(track+4) ? 4:3;
    if(!n) return fallback;
    auto v=[values,first](size_t i) { return double(disk<T>(values+(first+i)*sizeof(T))); };
    if(n==1) return v(0);
    if(frame<=v(0)) return v(1);
    if(frame>=v((n-1)*stride)) return v((n-1)*stride+1);
    size_t k=0; while(frame>=v((k+1)*stride)) ++k;
    const size_t a=k*stride,b=(k+1)*stride;
    const double d=v(b)-v(a),t=(frame-v(a))/d;
    return (2*t*t*t-3*t*t+1)*v(a+1)+(t*t*t-2*t*t+t)*d*v(a+stride-1)
        +(-2*t*t*t+3*t*t)*v(b+1)+(t*t*t-t*t)*d*v(b+2);
}
static size_t checkTexture(const unsigned char* block,size_t size) {
    J3DAnmTextureSRTKey animation;
    require(p2_load_texture_animation(&animation,block,size),"TTK1 texture animation loads");
    size_t samples=0;
    for(size_t i=0;i<animation.getUpdateMaterialNum();++i) {
        require(animation.mUpdateMaterialName.getName(i),"texture material name");
        require(animation.mUpdateMaterialID[i]==disk<u16>(block+disk<u32>(block+24)+i*2),"texture material ID");
        require(animation.mUpdateTexMtxID[i]==block[disk<u32>(block+32)+i],"texture matrix ID");
        const auto* center=block+disk<u32>(block+36)+i*12;
        require(animation.mSRTCenter[i].x==disk<float>(center) && animation.mSRTCenter[i].y==disk<float>(center+4)
            && animation.mSRTCenter[i].z==disk<float>(center+8),"texture pivot conversion");
        const auto* table=block+disk<u32>(block+20)+i*54;
        const auto* scales=block+disk<u32>(block+40), *rotations=block+disk<u32>(block+44), *translations=block+disk<u32>(block+48);
        for(float frame:{-1.0f,0.0f,0.5f,2.5f,5.0f,float(animation.getTotalFrameCount())+1}) {
            animation.setFrame(frame); J3DTextureSRTInfo info{}; animation.getTransform(i,&info);
            const double expected[]={sampleDiskKey<float>(table,scales,frame,1),sampleDiskKey<float>(table+18,scales,frame,1),
                sampleDiskKey<float>(table+12,translations,frame,0),sampleDiskKey<float>(table+30,translations,frame,0)};
            const float actual[]={info.mScaleX,info.mScaleY,info.mTranslationX,info.mTranslationY};
            for(int j=0;j<4;++j) require(std::fabs(actual[j]-expected[j])<=1e-4*(1+std::fabs(expected[j])),"texture transform matches independent disk sampler");
            const s16 rotation=static_cast<s16>(int(sampleDiskKey<s16>(table+42,rotations,frame,0))*(1<<block[9]));
            require(std::abs(int(info.mRotation)-rotation)<=(1<<block[9]),"texture rotation scaling");
            ++samples;
        }
    }
    auto* storage=animation.mNativeStorage;
    require(!p2_load_texture_animation(&animation,block,32) && animation.mNativeStorage==storage,"truncated TTK1 preserves live data");
    require(p2_load_texture_animation(&animation,block,size),"texture animation replacement");
    return samples;
}
static void knownTextureCurve() {
    unsigned char b[240]={};
    store<u32>(b,0x54544b31); store<u32>(b+4,sizeof(b)); b[9]=2; store<u16>(b+10,10);
    store<u16>(b+12,3);
    for(int c=0;c<3;++c) store<u16>(b+14+c*2,6);
    const u32 offsets[]={96,150,152,164,168,180,204,216};
    for(int i=0;i<8;++i) store<u32>(b+20+i*4,offsets[i]);
    for(int axis=0;axis<3;++axis) for(int c=0;c<3;++c) store<u16>(b+96+axis*18+c*6,2);
    store<u16>(b+150,0x1234); store<u16>(b+152,1); store<u16>(b+156,97); store<u16>(b+158,8); b[160]='a'; b[164]=3;
    for(int i=0;i<3;++i) store<float>(b+168+i*4,0.25f*(i+1));
    const float scales[]={0,1,0,10,3,0}, translations[]={0,-10,0,10,10,0};
    const s16 rotations[]={0,-100,0,10,100,0};
    for(int i=0;i<6;++i) { store<float>(b+180+i*4,scales[i]); store<s16>(b+204+i*2,rotations[i]); store<float>(b+216+i*4,translations[i]); }
    require(checkTexture(b,sizeof(b))==6,"analytic texture curve");
    // Exercise post-transform tables with aliased source arrays.
    std::memcpy(b+52,b+12,8); std::memcpy(b+60,b+20,32); store<u32>(b+92,1);
    J3DAnmTextureSRTKey a;
    require(p2_load_texture_animation(&a,b,sizeof(b)) && a.getPostUpdateMaterialNum()==1,"post-transform load");
    require(a.mPostUpdateMaterialID[0]==0x1234 && a.mPostUpdateTexMtxID[0]==3 && a.mPostSRTCenter[0].z==0.75f
        && a._4C[4]==3 && a._50[4]==100 && a._54[4]==10 && a.mTransformKeyTable[0].mScaleInfo.mMaxFrame==2,"post-transform arrays decode");
    a.mUpdateMaterialID[0]=7;
    require(a.mPostUpdateMaterialID[0]==0x1234 && disk<u16>(b+150)==0x1234,"aliased disk tables have independent writable native arrays");
    auto* storage=a.mNativeStorage;
    store<u16>(b+12,2);
    require(!p2_load_texture_animation(&a,b,sizeof(b)) && a.mNativeStorage==storage,"incomplete axis triplet rejected");
    store<u16>(b+12,3); store<u16>(b+98,0xffff);
    require(!p2_load_texture_animation(&a,b,sizeof(b)),"texture key range rejected");
    store<u16>(b+98,0); store<float>(b+192,0);
    require(!p2_load_texture_animation(&a,b,sizeof(b)),"duplicate float key time rejected");
    store<float>(b+192,10); store<float>(b+168,NAN);
    require(!p2_load_texture_animation(&a,b,sizeof(b)),"nonfinite texture pivot rejected");
}
static size_t checkMenuColor(const unsigned char* b,size_t size) {
    J2DAnmColorKey animation;
    require(p2_load_2d_color(&animation,b,size),"PAK1 menu color loads");
    size_t samples=0;
    for(size_t i=0;i<animation.getUpdateMaterialNum();++i) {
        require(animation.mNameTab.getName(i) && animation.mNameTab.getIndex(animation.mNameTab.getName(i))>=0,"owned menu material name lookup");
        require(animation.mUpdateMaterialID[i]==disk<u16>(b+disk<u32>(b+28)+i*2),"menu material ID conversion");
        for(float frame:{-1.0f,0.0f,0.5f,2.5f,98.5f,99.0f,99.5f,float(animation.getFrameMax())*0.37f,float(animation.getFrameMax())+1}) {
            animation.setFrame(frame); GXColor result{}; animation.getColor(i,&result);
            const int actual[]={result.r,result.g,result.b,result.a};
            for(size_t c=0;c<4;++c) {
                const auto* track=b+disk<u32>(b+24)+i*24+c*6;
                double expected=sampleDiskKey<s16>(track,b+disk<u32>(b+36+c*4),frame,0);
                if(disk<u16>(track)>1) expected=std::fmax(0,std::fmin(255,expected));
                const int quantized=static_cast<u8>(static_cast<int>(expected));
                require(std::abs(actual[c]-quantized)<=1,"original J2D color evaluator matches independent disk-key curve");
            }
            ++samples;
        }
    }
    auto* storage=animation.mNativeStorage;
    require(!p2_load_2d_color(&animation,b,12) && animation.mNativeStorage==storage,"invalid color load preserves live data");
    require(p2_load_2d_color(&animation,b,size),"menu color replacement");
    return samples;
}
static void knownMenuColor() {
    unsigned char b[108]={};
    store<u32>(b,0x50414b31); store<u32>(b+4,sizeof(b)); store<u16>(b+12,10); store<u16>(b+14,1);
    store<u32>(b+24,52); store<u32>(b+28,76); store<u32>(b+32,78); store<u16>(b+76,0x1234);
    store<u16>(b+78,1); store<u16>(b+82,97); store<u16>(b+84,8); b[86]='a';
    for(int c=0;c<4;++c) { store<u16>(b+16+c*2,6); store<u32>(b+36+c*4,96); store<u16>(b+52+c*6,2); }
    const s16 keys[]={0,-100,0,10,300,0};
    for(int i=0;i<6;++i) store<s16>(b+96+i*2,keys[i]);
    require(checkMenuColor(b,sizeof(b))==9,"menu color interpolation and clamp checks");
    J2DAnmColorKey a;
    require(p2_load_2d_color(&a,b,sizeof(b)),"known menu color");
    a.mUpdateMaterialID[0]=7; b[86]='z';
    require(disk<u16>(b+76)==0x1234 && !std::strcmp(a.mNameTab.getName(0),"a"),"menu bindings and names have independent ownership");
    auto* storage=a.mNativeStorage;
    store<u16>(b+54,0xffff);
    require(!p2_load_2d_color(&a,b,sizeof(b)) && a.mNativeStorage==storage,"invalid color channel range preserves storage");
    store<u16>(b+54,0); store<s16>(b+102,-1);
    require(!p2_load_2d_color(&a,b,sizeof(b)),"decreasing color time rejected");
    store<s16>(b+102,10); store<u16>(b+84,0xffff);
    require(!p2_load_2d_color(&a,b,sizeof(b)),"invalid color name offset rejected");
}
int main(int argc, const char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024),"disc and memory");
    auto* heap=JKRExpHeap::createRoot(16,false);
    require(heap && p2_dvd_mount(argv[1]),"heap and DVD");
    knownCurve();
    knownTevCurve();
    knownTextureCurve();
    knownMenuColor();
    require(std::fabs(JMAHermiteInterpolation(5,0,2,0,10,8,0)-5)<1e-6f,"float Hermite midpoint");
    s16 t0=0,v0=-100,d0=0,t1=10,v1=100,d1=0;
    require(std::fabs(J3DHermiteInterpolation<s16>(5,&t0,&v0,&d0,&t1,&v1,&d1))<1e-6f,"integer Hermite midpoint");
    size_t animations=0, samples=0, colors=0, colorSamples=0, textures=0, textureSamples=0, menuColors=0, menuColorSamples=0;
    for (const char* path : {"new_screen/eng/title.szs", "new_screen/eng/omake.szs", "user/Ebisawa/title/title.szs", "user/Ebisawa/title/bg_spring.szs",
                            "user/Ebisawa/title/bg_summer.szs", "user/Ebisawa/title/bg_autumn.szs",
                            "user/Ebisawa/title/bg_winter.szs", "user/Kando/piki/pikis.szs"}) {
        auto reference=p2::decompressYaz0(p2::readAsset(std::filesystem::path(argv[1])/"files",path));
        auto* archive=JKRArchive::mount(path,JKRArchive::EMM_Mem,heap,JKRArchive::EMD_Head);
        require(archive,"animation archive");
        for (const auto& entry:p2::readRarc(reference)) {
            auto* bytes=static_cast<const unsigned char*>(archive->getResource(("/"+entry.path).c_str()));
            if(entry.size<68 || std::memcmp(bytes,"J3D1",4)) continue;
            if (!std::memcmp(bytes+4,"bpk1",4)) {
                const u32 before=heap->getTotalFreeSize();
                menuColorSamples+=checkMenuColor(bytes+32,entry.size-32); ++menuColors;
                require(heap->check() && heap->getTotalFreeSize()==before,"menu color storage teardown restores heap");
                continue;
            }
            if (!std::memcmp(bytes+4,"btk1",4)) {
                const u32 before=heap->getTotalFreeSize();
                textureSamples+=checkTexture(bytes+32,entry.size-32); ++textures;
                require(heap->check() && heap->getTotalFreeSize()==before,"texture storage teardown restores heap");
                continue;
            }
            if (!std::memcmp(bytes+4,"brk1",4)) {
                const u32 before=heap->getTotalFreeSize();
                colorSamples+=checkTev(bytes+32,entry.size-32); ++colors;
                require(heap->check() && heap->getTotalFreeSize()==before,"TEV storage teardown restores heap");
                continue;
            }
            bool key=!std::memcmp(bytes+4,"bck1",4);
            if(!key && std::memcmp(bytes+4,"bca1",4)) continue;
            static_assert(sizeof(J2DAnmDataHeader)==40,"J2D animation disk header layout");
            const auto* header=reinterpret_cast<const J2DAnmDataHeader*>(bytes);
            require(header->mMagic==0x4a334431 && header->mCount==1 &&
                    header->mFirst.mType==(key ? 0x414e4b31u:0x414e4631u) && header->mFirst.mNextOffset==disk<u32>(bytes+36),
                    "original J2D loader sees decoded file and block headers");
            const u32 before=heap->getTotalFreeSize();
            auto* animation=key ? static_cast<J3DAnmTransform*>(new J3DAnmTransformKey) : static_cast<J3DAnmTransform*>(new J3DAnmTransformFull);
            if(!p2_load_transform(animation,bytes+32,entry.size-32,key)) {
                std::fprintf(stderr,"Rejected %s/%s (%zu bytes)\n",path,entry.path.c_str(),entry.size);
                for(int i=32;i<68;++i) std::fprintf(stderr,"%02x%s",bytes[i],(i+1)%4 ? " " : "\n");
                return 1;
            }
            auto* menu=key ? static_cast<J2DAnmTransform*>(new J2DAnmTransformKey) : static_cast<J2DAnmTransform*>(new J2DAnmTransformFull);
            require(p2_load_2d_transform(menu,bytes+32,entry.size-32,key),"original J2D transform loader conversion");
            require(animation->getTotalFrameCount()>0,"animation duration");
            for(float frame:{-1.0f,0.0f,0.5f,float(animation->getTotalFrameCount())*0.37f,float(animation->getTotalFrameCount())+1}) {
                animation->setFrame(frame); menu->setFrame(frame);
                for(u16 joint=0;joint<animation->mUpdateMaterialNum;++joint) {
                    J3DTransformInfo info{}, menuInfo{}; animation->getTransform(joint,&info); menu->getTransform(joint,&menuInfo);
                    J3DTransformInfo expectedInfo=info;
                    if(!key) {
                        // J2D truncates full-animation frames; J3D rounds them.
                        animation->setFrame(std::floor(frame)); animation->getTransform(joint,&expectedInfo); animation->setFrame(frame);
                    }
                    require(expectedInfo.mScale.x==menuInfo.mScale.x && expectedInfo.mScale.y==menuInfo.mScale.y && expectedInfo.mScale.z==menuInfo.mScale.z
                        && expectedInfo.mRotation.x==menuInfo.mRotation.x && expectedInfo.mRotation.y==menuInfo.mRotation.y && expectedInfo.mRotation.z==menuInfo.mRotation.z
                        && expectedInfo.mTranslation.x==menuInfo.mTranslation.x && expectedInfo.mTranslation.y==menuInfo.mTranslation.y && expectedInfo.mTranslation.z==menuInfo.mTranslation.z,
                        "original J2D and J3D evaluators agree on converted transform channels");
                    require(std::isfinite(info.mScale.x)&&std::isfinite(info.mScale.y)&&std::isfinite(info.mScale.z)&&
                            std::isfinite(info.mTranslation.x)&&std::isfinite(info.mTranslation.y)&&std::isfinite(info.mTranslation.z),"finite original animation evaluation");
                    ++samples;
                }
            }
            auto* storage=animation->mNativeStorage;
            require(!p2_load_transform(animation,bytes+32,12,key) && animation->mNativeStorage==storage,"truncated input preserves existing animation");
            require(p2_load_transform(animation,bytes+32,entry.size-32,key),"animation replacement");
            auto* menuStorage=menu->mNativeStorage;
            require(!p2_load_2d_transform(menu,bytes+32,12,key) && menu->mNativeStorage==menuStorage,"bad 2D animation preserves live storage");
            require(p2_load_2d_transform(menu,bytes+32,entry.size-32,key),"2D animation replacement");
            delete menu;
            delete animation;
            require(heap->check() && heap->getTotalFreeSize()==before,"animation destruction releases converted data");
            ++animations;
        }
        archive->unmount();
    }
    // J2D full visibility (bva): the only users on disc are the Challenge result screens.
    size_t visibilities=0, visibilitySamples=0;
    {
        const char* path="new_screen/eng/res_challengeResult.szs";
        auto reference=p2::decompressYaz0(p2::readAsset(std::filesystem::path(argv[1])/"files",path));
        auto* archive=JKRArchive::mount(path,JKRArchive::EMM_Mem,heap,JKRArchive::EMD_Head);
        require(archive,"visibility archive");
        for (const auto& entry:p2::readRarc(reference)) {
            auto* bytes=static_cast<const unsigned char*>(archive->getResource(("/"+entry.path).c_str()));
            if(entry.size<56 || std::memcmp(bytes,"J3D1bva1",8)) continue;
            const unsigned char* block=bytes+32;
            const u32 before=heap->getTotalFreeSize();
            auto* animation=new J2DAnmVisibilityFull;
            require(p2_load_2d_visibility(animation,block,entry.size-32),"original visibility data converts");
            const u16 tables=disk<u16>(block+12);
            const u32 table=disk<u32>(block+16), values=disk<u32>(block+20);
            require(animation->mAnimTableNum1==tables && animation->getFrameMax()==disk<s16>(block+10),"visibility header");
            for(float frame:{-1.0f,0.0f,1.0f,float(animation->getFrameMax())*0.5f,float(animation->getFrameMax())+3}) {
                animation->setFrame(frame);
                for(u16 i=0;i<tables;++i) {
                    const int count=disk<u16>(block+table+i*4), first=disk<u16>(block+table+i*4+2);
                    const int index=frame<0 ? 0 : std::min(int(frame),count-1);
                    u8 visible=0xEE; animation->getVisibility(i,&visible);
                    require(visible==block[values+first+index],"visibility matches the disc table");
                    ++visibilitySamples;
                }
            }
            auto* storage=animation->mNativeStorage;
            require(!p2_load_2d_visibility(animation,block,12) && animation->mNativeStorage==storage,"truncated visibility preserves live storage");
            delete animation;
            require(heap->check() && heap->getTotalFreeSize()==before,"visibility storage teardown restores heap");
            ++visibilities;
        }
        archive->unmount();
        require(visibilities>0,"Challenge result screen carries visibility animations");
    }
    for (const auto& entry:std::filesystem::recursive_directory_iterator(std::filesystem::path(argv[1])/"files/enemy/data")) {
        if(entry.path().extension()!=".btk") continue;
        auto data=p2::readAsset(entry.path().parent_path(),entry.path().filename().string());
        const u32 before=heap->getTotalFreeSize();
        textureSamples+=checkTexture(data.data()+32,data.size()-32); ++textures;
        require(heap->check() && heap->getTotalFreeSize()==before,"retail texture storage teardown");
    }
    require(menuColors>=5 && menuColorSamples>0,"title and bonus-screen color animations covered");
    std::printf("Menu colors: %zu files, %zu material/frame samples.\n",menuColors,menuColorSamples);
    require(textures>0 && textureSamples>0,"retail texture animations covered");
    std::printf("Texture animations: %zu files, %zu material/frame samples.\n",textures,textureSamples);
    require(colors>0 && colorSamples>0,"retail title TEV animations covered");
    std::printf("TEV animations: %zu files, %zu material/frame samples.\n",colors,colorSamples);
    require(animations>10 && samples>100,"real animation coverage");
    std::printf("Original J3D animation evaluator: %zu animations, %zu joint/frame samples.\n",animations,samples);
    std::printf("J2D visibility animations: %zu files, %zu table/frame samples.\n",visibilities,visibilitySamples);
}
