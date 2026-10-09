#include "JSystem/J3D/J3DMaterialFactory.h"
#include "JSystem/J3D/J3DColorBlock.h"
#include "JSystem/JSupport/JSUStream.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "p2_material.h"
#include "p2_assets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {
void require(bool value,const char* what) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
float readFloat(JSUMemoryInputStream& stream) {
    const u32 bits=stream.readU32(); float value; std::memcpy(&value,&bits,4); return value;
}
}
size_t checkMaterial(const J3DMaterialBlock* block) {
    const auto* bytes=reinterpret_cast<const u8*>(block);
    const u32 freeBefore=JKRHeap::getCurrentHeap()->getTotalFreeSize();
    {
        J3DMaterialFactory factory(*block);
        require(factory.mMaterialNum==block->mNumMaterials,"original material factory count");
        JSUMemoryInputStream remaps(bytes+u32(block->mMatRemapTableOffset),2*factory.mMaterialNum);
        for(u16 n=0;n<factory.mMaterialNum;++n) {
            const u16 mapped=remaps.readU16();
            require(factory.getMaterialID(n)==mapped,"original material factory remap");
            const auto& init=factory.getMaterialInitData(n);
            const auto* raw=bytes+u32(block->mMatEntryDataOffset)+mapped*0x14c;
            JSUMemoryInputStream disk(raw,0x14c);
            const auto* native=reinterpret_cast<const u8*>(&init);
            for(unsigned at=0;at<0x14c;) {
                if((at>=8 && at<0x9c) || at>=0xbc) {
                    u16 value; std::memcpy(&value,native+at,2);
                    require(value==disk.readU16(),"every material initializer index matches independent stream read"); at+=2;
                } else { require(native[at]==disk.readByte(),"material byte fields preserved"); ++at; }
            }
            require(factory.countTexGens(n)<=8 && factory.countStages(n)<=16,"original material stage counts");
            alignas(32) u8 commands[64]={};
            GDCurrentDL dl;
            auto* previous=__GDCurrentDL;
            for(unsigned stageIndex=0;stageIndex<factory.countStages(n);++stageIndex) {
                auto stage=factory.newTevStage(n,stageIndex);
                stage.mBPCommand1=0xc0+2*stageIndex; stage.mBPCommand2=0xc1+2*stageIndex;
                GDInitGDLObj(&dl,commands,sizeof(commands)); __GDSetCurrent(&dl);
                stage.load(stageIndex);
                require(dl.data-commands==10 && commands[0]==0x61 && commands[5]==0x61 &&
                        !std::memcmp(commands+1,&stage.mBPCommand1,4) && !std::memcmp(commands+6,&stage.mBPCommand2,4),
                        "original TEV stage emits big-endian packed register commands");
            }
            J3DGXColor colors[2]={factory.newMatColor(n,0),factory.newMatColor(n,1)};
            GDInitGDLObj(&dl,commands,sizeof(commands)); __GDSetCurrent(&dl);
            loadMatColors(colors);
            require(dl.data-commands==13 && commands[0]==0x10 && !std::memcmp(commands+5,colors,8),"material color commands retain RGBA channel order");
            GDInitGDLObj(&dl,commands,sizeof(commands));
            __GDWriteF32(1.0f); __GDWriteF32(-2.5f);
            const u8 floats[]={0x3f,0x80,0,0,0xc0,0x20,0,0};
            require(!std::memcmp(commands,floats,8),"SDK float command writer preserves console byte order");
            u8 textureCommand[]={0x61,0x80,0x00,0x12,0x34};
            require(isTexNoReg(textureCommand) && getTexNoReg(textureCommand)==0x1234,"original texture command reader decodes unaligned big-endian register");
            __GDSetCurrent(previous);
            if(init.mCullModeIndex!=255) {
                JSUMemoryInputStream cull(bytes+u32(block->mCullModeInfoOffset)+4*init.mCullModeIndex,4);
                require(factory.newCullMode(n)==cull.readU32(),"original material culling mode");
            }
            for(unsigned t=0;t<8;++t) {
                const u16 id=init.mTextureIndex[t];
                if(id==65535) require(factory.newTexNo(n,t)==65535,"missing material texture sentinel");
                else {
                    JSUMemoryInputStream texture(bytes+u32(block->mTextureRemapTableOffset)+2*id,2);
                    require(factory.newTexNo(n,t)==texture.readU16(),"original material texture lookup");
                }
                auto* matrix=factory.newTexMtx(n,t);
                const u16 matrixIndex=init.mTexMatrixIndex[t];
                require((matrix!=nullptr)==(matrixIndex!=65535),"original texture matrix allocation");
                if(matrix) {
                    JSUMemoryInputStream tex(bytes+u32(block->mTexMtxInfoOffset)+100*matrixIndex,100);
                    const auto& info=matrix->mTexMtxInfo;
                    require(info.mProjection==tex.readByte() && info.mInfo==tex.readByte(),"texture matrix projection"); tex.readU16();
                    require(info.mCenter.x==readFloat(tex) && info.mCenter.y==readFloat(tex) && info.mCenter.z==readFloat(tex),"texture matrix center");
                    require(info.mSRT.mScaleX==readFloat(tex) && info.mSRT.mScaleY==readFloat(tex) && info.mSRT.mRotation==tex.readS16(),"texture matrix scale and rotation");
                    tex.readU16();
                    require(info.mSRT.mTranslationX==readFloat(tex) && info.mSRT.mTranslationY==readFloat(tex),"texture matrix translation");
                    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
                        require(info.mEffectMtx[r][c]==readFloat(tex),"texture effect matrix");
                    delete matrix;
                }
            }
            for(unsigned c=0;c<4;++c) {
                const auto color=factory.newTevColor(n,c);
                if(init.mTevColorIndex[c]!=65535) {
                    JSUMemoryInputStream tev(bytes+u32(block->mTevColorsOffset)+8*init.mTevColorIndex[c],8);
                    require(color.r==tev.readS16() && color.g==tev.readS16() && color.b==tev.readS16() && color.a==tev.readS16(),"original signed TEV colors");
                }
            }
            if(init.mFogInfoIndex!=65535) {
                const auto fog=factory.newFog(n);
                JSUMemoryInputStream diskFog(bytes+u32(block->mFogInfoOffset)+44*init.mFogInfoIndex,44);
                require(fog.mType==diskFog.readByte() && fog.mAdjEnable==diskFog.readByte() && fog.mCenter==diskFog.readU16(),"original material fog mode");
                require(fog.mStartZ==readFloat(diskFog) && fog.mEndZ==readFloat(diskFog) && fog.mNearZ==readFloat(diskFog) && fog.mFarZ==readFloat(diskFog),"original material fog distances");
                diskFog.skip(4);
                for(unsigned k=0;k<10;++k) require(fog.mFogAdjTable[k]==diskFog.readU16(),"original material fog adjustment table");
            }
            if(init.mNBTScaleIndex!=65535) {
                const auto scale=factory.newNBTScale(n);
                JSUMemoryInputStream nbt(bytes+u32(block->mNBTScaleInfoOffset)+16*init.mNBTScaleIndex,16);
                require(scale.mHasScale==nbt.readByte(),"original NBT scale flag"); nbt.skip(3);
                require(scale.mScale.x==readFloat(nbt) && scale.mScale.y==readFloat(nbt) && scale.mScale.z==readFloat(nbt),"original material NBT scale");
            }
            if(factory.mIndInitData && factory.newIndTexStageNum(n)) {
                for(unsigned m=0;m<3;++m) {
                    const auto matrix=factory.newIndTexMtx(n,m);
                    JSUMemoryInputStream ind(bytes+u32(block->mIndTextureInfoOffset)+n*0x138+0x14+m*28,28);
                    for(unsigned r=0;r<2;++r) for(unsigned c=0;c<3;++c)
                        require(matrix.mOffsetMtx[r][c]==readFloat(ind),"original indirect texture matrix");
                    require(matrix.mScaleExp==s8(ind.readByte()),"indirect matrix exponent");
                }
            }
        }
        require(!p2_decode_material(block,32),"truncated material block rejected");
        p2::Bytes malformed(bytes,bytes+u32(block->mSize));
        auto* bad=reinterpret_cast<J3DMaterialBlock*>(malformed.data());
        bad->mMatRemapTableOffset=u32(block->mSize);
        require(!p2_decode_material(bad,malformed.size()),"material remap outside block rejected");
        std::memcpy(malformed.data(),bytes,malformed.size());
        const size_t init=u32(block->mMatEntryDataOffset)+factory.getMaterialID(0)*0x14c;
        malformed[init+0x84]=0xff; malformed[init+0x85]=0xfe;
        require(!p2_decode_material(bad,malformed.size()),"invalid material texture-table index rejected");
    }
    require(JKRHeap::getCurrentHeap()->getTotalFreeSize()==freeBefore,"material factory and texture matrices release owned data");
    return block->mNumMaterials;
}
