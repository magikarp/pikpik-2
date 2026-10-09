#include "JSystem/J3D/J3DFileBlock.h"
#include "JSystem/J3D/J3DJointFactory.h"
#include "JSystem/J3D/J3DVertexData.h"
#include "JSystem/J3D/J3DModel.h"
#include "JSystem/J3D/J3DTexture.h"
#include "JSystem/J3D/J3DShapeFactory.h"
#include "p2_shape.h"
#include "p2_texture_refs.h"
#include "p2_vertex.h"
#include "p2_skin.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JSupport/JSU.h"
#include "JSystem/JSupport/JSUStream.h"
#include "JSystem/JUtility/JUTNameTab.h"
#include "p2_assets.h"
#include "p2_memory.h"
#include "p2_dvd.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
size_t checkMaterial(const J3DMaterialBlock*);

static void require(bool value, const char* what) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void offset(const J3DFileBlockBase* block, u32 value) {
    require(value == 0 || (value >= 8 && value < u32(block->mSize)), "J3D block-relative offset bounds");
    require(JSUConvertOffsetToPtr<u8>(block, value) == (value ? reinterpret_cast<const u8*>(block)+value : nullptr),
            "J3D disk offset resolves without losing native address bits");
}
static void names(const J3DFileBlockBase* block, u32 value, u16 expected) {
    offset(block, value);
    if (!value) return;
    auto* table = JSUConvertOffsetToPtr<ResNTAB>(block, value);
    require(value+4u+u32(table->mEntryNum)*4u <= u32(block->mSize), "name table entries fit block");
    JUTNameTab lookup(table);
    require(lookup.mNameNum == expected, "name table count matches owning block");
    for (u16 i=0; i<lookup.mNameNum; ++i) {
        u32 nameAt = value+table->mEntries[i].mOffs;
        require(nameAt < u32(block->mSize), "name table string offset bounds");
        require(std::memchr(reinterpret_cast<const u8*>(block)+nameAt, 0, u32(block->mSize)-nameAt), "name terminator");
        const char* name = lookup.getName(i);
        require(name && lookup.getIndex(name) >= 0, "original hashed name lookup");
    }
    require(!lookup.getName(lookup.mNameNum) && lookup.getIndex("_missing_pikmin_native_name_") == -1, "name table misses");
}
int main(int argc, const char** argv) {
    static_assert(sizeof(J3DDrawBlock) == 0x14, "DRW1 disk width");
    static_assert(sizeof(J3DEnvelopeBlock) == 0x1c, "EVP1 disk width");
    static_assert(sizeof(J3DJointBlock) == 0x18, "JNT1 disk width");
    static_assert(sizeof(J3DMaterialBlock) == 0x84, "MAT3 disk width");
    static_assert(sizeof(J3DMaterialBlock_v21) == 0x78, "MAT2 disk width");
    static_assert(sizeof(J3DModelInfoBlock) == 0x18, "INF1 disk width");
    static_assert(sizeof(J3DShapeBlock) == 0x2c, "SHP1 disk width");
    static_assert(sizeof(J3DTextureBlock) == 0x14, "TEX1 disk width");
    static_assert(sizeof(J3DVertexBlock) == 0x40, "VTX1 disk width");
    static_assert(sizeof(J3DModelHierarchy) == 4, "INF1 hierarchy disk width");
    static_assert(sizeof(ResTIMG) == 32, "texture header disk width");
    require(argc == 2 && p2_memory_init(128*1024*1024), "disc argument and memory");
    auto* heap = JKRExpHeap::createRoot(16, false);
    require(heap && p2_dvd_mount(argv[1]), "heap and DVD mount");
    size_t models = 0, blocks = 0, joints = 0, vertices = 0, influences = 0, hierarchyNodes = 0, drawMatrices = 0, textures = 0, shapes = 0, matrixGroups = 0, materials = 0;
    for (const char* path : {"user/Ebisawa/title/title.szs", "user/Ebisawa/title/bg_spring.szs",
                            "user/Ebisawa/title/bg_summer.szs", "user/Ebisawa/title/bg_autumn.szs",
                            "user/Ebisawa/title/bg_winter.szs"}) {
        const auto reference = p2::decompressYaz0(p2::readAsset(std::filesystem::path(argv[1])/"files", path));
        auto* archive = JKRArchive::mount(path, JKRArchive::EMM_Mem, heap, JKRArchive::EMD_Head);
        require(archive != nullptr, "actual title scene archive mount");
        for (const auto& entry : p2::readRarc(reference)) {
            auto* data = static_cast<const u8*>(archive->getResource(("/"+entry.path).c_str()));
            if (entry.size < 32 || std::memcmp(data, "J3D", 3) ||
                (std::memcmp(data+4, "bmd", 3) && std::memcmp(data+4, "bdl", 3))) continue;
            p2::readJ3dChunks(p2::Bytes(data, data+entry.size));
            auto* header = reinterpret_cast<const J3DFileHeader*>(data);
            require(header->mJ3dVersion == 0x4a334432, "J3D2 header conversion");
            auto* block = header->getFirstBlock();
            u16 jointCount=0, envelopeCount=0, materialCount=0, shapeCount=0;
            for (u32 i=0; i<header->mBlockCount; ++i, block=block->getNext()) {
                switch (block->mBlockType) {
                case J3DFBT_Joint: jointCount=static_cast<const J3DJointBlock*>(block)->mCount; break;
                case J3DFBT_Envelope: envelopeCount=static_cast<const J3DEnvelopeBlock*>(block)->mCount; break;
                case J3DFBT_Material: materialCount=static_cast<const J3DMaterialBlock*>(block)->mNumMaterials; break;
                case J3DFBT_Shape: shapeCount=static_cast<const J3DShapeBlock*>(block)->mShapeNum; break;
                }
            }
            block=header->getFirstBlock();
            u32 vertexCount=0;
            for (u32 i=0; i<header->mBlockCount; ++i, block=block->getNext()) {
                require(reinterpret_cast<const u8*>(block)+u32(block->mSize) <= data+entry.size, "model block bounds");
                switch (block->mBlockType) {
                case J3DFBT_Info: {
                    auto* b = static_cast<const J3DModelInfoBlock*>(block);
                    require(b->mVertexCount > 0, "model vertex count conversion");
                    vertexCount=b->mVertexCount;
                    offset(b, b->mHierarchyDataOffset);
                    const u32 bytes=u32(b->mSize)-u32(b->mHierarchyDataOffset);
                    auto* hierarchy=JSUConvertOffsetToPtr<J3DModelHierarchy>(b,b->mHierarchyDataOffset);
                    require(hierarchy,"model has hierarchy");
                    JSUMemoryInputStream disk(hierarchy,bytes);
                    int depth=0; bool terminated=false;
                    for(u32 n=0;n<bytes/sizeof(J3DModelHierarchy);++n) {
                        const u16 type=hierarchy[n].mType, value=hierarchy[n].mValue;
                        require(type==disk.readU16() && value==disk.readU16(),"hierarchy fields match independent stream decode");
                        if(type==0) { require(depth==0,"balanced model hierarchy"); terminated=true; break; }
                        if(type==1) ++depth;
                        else if(type==2) { require(depth>0,"hierarchy close has parent"); --depth; }
                        else if(type==0x10) require(value<jointCount,"hierarchy joint reference");
                        else if(type==0x11) require(value<materialCount,"hierarchy material reference");
                        else if(type==0x12) require(value<shapeCount,"hierarchy shape reference");
                        else require(false,"recognized hierarchy opcode");
                        ++hierarchyNodes;
                    }
                    require(terminated,"bounded hierarchy terminator");
                    break;
                }
                case J3DFBT_Draw: {
                    auto* b=static_cast<const J3DDrawBlock*>(block);
                    const u32 count=b->mCount;
                    require(count>=envelopeCount,"draw count includes envelopes");
                    offset(b,b->mMatrixTypeArrayOffset); offset(b,b->mDataArrayOffset);
                    require(u32(b->mMatrixTypeArrayOffset)+count<=u32(b->mSize) &&
                            u32(b->mDataArrayOffset)+2*count<=u32(b->mSize),"draw matrix table bounds");
                    J3DDrawMtxData draw;
                    draw.mDrawMtxFlag=JSUConvertOffsetToPtr<u8>(b,b->mMatrixTypeArrayOffset);
                    draw.mDrawMtxIdx=JSUConvertOffsetToPtr<P2Big<u16>>(b,b->mDataArrayOffset);
                    JSUMemoryInputStream disk(draw.mDrawMtxIdx,count*2);
                    for(u32 n=0;n<count;++n) {
                        const u16 index=draw.mDrawMtxIdx[n];
                        require(index==disk.readU16(),"draw matrix index matches independent stream decode");
                        require(draw.mDrawMtxFlag[n]<=1,"draw matrix kind");
                        require(index<(draw.mDrawMtxFlag[n] ? envelopeCount : jointCount),"draw matrix references existing joint or envelope");
                        ++drawMatrices;
                    }
                    break;
                }
                case J3DFBT_Vertex: {
                    auto* b = static_cast<const J3DVertexBlock*>(block);
                    offset(b, b->mVertexFormatOffset); offset(b, b->mPositionDataOffset);
                    offset(b, b->mNormalDataOffset); offset(b, b->mNBTDataOffset);
                    for (auto value : b->mColorDataOffset) offset(b, value);
                    for (auto value : b->mTexCoordDataOffset) offset(b, value);
                    const u32 freeBefore=heap->getTotalFreeSize();
                    {
                        J3DVertexData vertex;
                        vertex.mVtxNum=vertexCount;
                        require(p2_load_vertex(&vertex,b,b->mSize),"native vertex payload conversion");
                        require(vertex.mVtxPos && vertex.mVtxAttrFmtList,"native position array and descriptor");
                        const GXVtxAttrFmtList* position=vertex.mVtxAttrFmtList;
                        while(position->mAttr!=GX_VA_NULL && position->mAttr!=GX_VA_POS) ++position;
                        require(position->mAttr==GX_VA_POS,"position descriptor decoded");
                        JSUMemoryInputStream disk(JSUConvertOffsetToPtr<void>(b,b->mPositionDataOffset),vertex.mNativeArrayBytes[0]);
                        const u32 components=position->mCount==GX_POS_XYZ ? 3 : 2;
                        for(u32 n=0;n<vertexCount*components;++n) {
                            if(position->mType==GX_F32) {
                                const u32 bits=disk.readU32(); float expected; std::memcpy(&expected,&bits,4);
                                require(static_cast<float*>(vertex.mVtxPos)[n]==expected,"native float position matches disk");
                            } else if(position->mType==GX_S16 || position->mType==GX_U16)
                                require(static_cast<u16*>(vertex.mVtxPos)[n]==disk.readU16(),"native fixed-point position matches disk");
                            else require(static_cast<u8*>(vertex.mVtxPos)[n]==disk.readByte(),"native byte position matches disk");
                        }
                        require(!p2_load_vertex(&vertex,b,32),"truncated vertex block rejected");
                        vertices+=vertexCount;
                    }
                    require(heap->getTotalFreeSize()==freeBefore,"converted geometry is released with vertex data");
                    break;
                }
                case J3DFBT_Joint: {
                    auto* b = static_cast<const J3DJointBlock*>(block);
                    require(b->mCount > 0, "joint count conversion");
                    offset(b,b->mJointInitData); offset(b,b->mRemapTableOffset); names(b,b->mNameTableOffset,b->mCount);
                    J3DJointFactory factory(*b);
                    for (u16 j=0;j<b->mCount;++j) {
                        auto* joint=factory.create(j);
                        const u16 record=p2_big_endian(factory.mIndexMap[j]);
                        require(u32(b->mJointInitData)+u32(record+1)*64 <= u32(b->mSize),"joint record bounds");
                        JSUMemoryInputStream disk(factory.mInitData+record,64);
                        auto readFloat=[&] { const u32 bits=disk.readU32(); f32 value; std::memcpy(&value,&bits,4); return value; };
                        const u16 kind=disk.readU16(); const u8 compensation=disk.readByte(); disk.readByte();
                        require(joint->mJointIdx==j && joint->mKind==s8(kind) && joint->mScaleCompensate==(compensation==255 ? 0 : compensation),"joint identity and scale inheritance");
                        require(joint->mTransformInfo.mScale.x==readFloat() && joint->mTransformInfo.mScale.y==readFloat() &&
                                joint->mTransformInfo.mScale.z==readFloat(),"original joint factory decodes scale");
                        require(joint->mTransformInfo.mRotation.x==disk.readS16() && joint->mTransformInfo.mRotation.y==disk.readS16() &&
                                joint->mTransformInfo.mRotation.z==disk.readS16(),"original joint factory decodes rotation");
                        disk.readU16();
                        require(joint->mTransformInfo.mTranslation.x==readFloat() && joint->mTransformInfo.mTranslation.y==readFloat() &&
                                joint->mTransformInfo.mTranslation.z==readFloat(),"original joint factory decodes translation");
                        require(joint->mBoundingSphereRadius==readFloat(),"joint bounding radius");
                        require(joint->mMin.x==readFloat() && joint->mMin.y==readFloat() && joint->mMin.z==readFloat() &&
                                joint->mMax.x==readFloat() && joint->mMax.y==readFloat() && joint->mMax.z==readFloat(),"joint bounding box");
                        delete joint; ++joints;
                    }
                    break;
                }
                case J3DFBT_Envelope: {
                    auto* b=static_cast<const J3DEnvelopeBlock*>(block);
                    const u32 freeBefore=heap->getTotalFreeSize();
                    auto* skin=p2_load_envelope(b,b->mSize);
                    require(skin && skin->count==b->mCount,"native skin envelope conversion");
                    size_t influence=0;
                    for(u16 group=0;group<skin->count;++group) {
                        float total=0;
                        for(u8 n=0;n<skin->counts[group];++n) {
                            require(skin->indices[influence]<skin->matrices,"envelope index references inverse bind matrix");
                            total+=skin->weights[influence++];
                        }
                        require(std::fabs(total-1.0f)<0.0001f,"skin weights sum to one");
                    }
                    require(influence==skin->influences,"envelope influence count");
                    if(skin->matrices) {
                        JSUMemoryInputStream disk(JSUConvertOffsetToPtr<void>(b,b->mInvBindTableOffset),skin->matrices*48);
                        for(u32 matrix=0;matrix<skin->matrices;++matrix) for(int row=0;row<3;++row) for(int col=0;col<4;++col) {
                            const u32 bits=disk.readU32(); float expected; std::memcpy(&expected,&bits,4);
                            require(skin->inverseBind[matrix][row][col]==expected && std::isfinite(expected),"inverse bind matrix matches disk");
                        }
                    }
                    influences+=skin->influences;
                    p2_game_free(skin);
                    require(!p2_load_envelope(b,12) && heap->getTotalFreeSize()==freeBefore,"skin buffer cleanup and truncated block rejection");
                    break;
                }
                case J3DFBT_Material: {
                    auto* b = static_cast<const J3DMaterialBlock*>(block);
                    require(b->mNumMaterials > 0, "material count conversion");
                    offset(b,b->mMatEntryDataOffset); names(b,b->mStringTableOffset,b->mNumMaterials); offset(b,b->mNBTScaleInfoOffset);
                    materials+=checkMaterial(b);
                    break;
                }
                case J3DFBT_Shape: {
                    auto* b = static_cast<const J3DShapeBlock*>(block);
                    require(b->mShapeNum > 0, "shape count conversion");
                    offset(b,b->mShapeDataOffset); offset(b,b->mPrimDataOffset); offset(b,b->mMtxGroupTableOffset);
                    const u32 freeBefore=heap->getTotalFreeSize();
                    {
                        J3DShapeFactory factory(*b);
                        const auto* original=reinterpret_cast<const u8*>(b);
                        JSUMemoryInputStream remap(original+u32(b->mRemapTableOffset),b->mShapeNum*2);
                        for(u16 n=0;n<b->mShapeNum;++n) {
                            const u16 mapped=remap.readU16();
                            require(factory.mInitDataIndices[n]==mapped,"shape remap index matches disk");
                            JSUMemoryInputStream shapeDisk(original+u32(b->mShapeDataOffset)+mapped*40,40);
                            const u8 type=shapeDisk.readByte(); shapeDisk.readByte();
                            const u16 groups=shapeDisk.readU16(), desc=shapeDisk.readU16(), firstMtx=shapeDisk.readU16(), firstDraw=shapeDisk.readU16();
                            require(factory.getMtxGroupNum(n)==groups && factory.mInitData[mapped].mShapeMtxType==type,"original factory reads native shape matrix groups");
                            shapeDisk.readU16();
                            auto readFloat=[&] { const u32 bits=shapeDisk.readU32(); float value; std::memcpy(&value,&bits,4); return value; };
                            require(factory.getRadius(n)==readFloat(),"original shape factory reads radius");
                            const auto& min=factory.getMin(n); const auto& max=factory.getMax(n);
                            require(min.x==readFloat() && min.y==readFloat() && min.z==readFloat() &&
                                    max.x==readFloat() && max.y==readFloat() && max.z==readFloat(),"original shape factory reads bounds");
                            auto* descriptors=factory.getVtxDescList(n);
                            JSUMemoryInputStream descDisk(original+u32(b->mAttribTableOffset)+desc,u32(b->mSize)-u32(b->mAttribTableOffset)-desc);
                            for(u32 d=0;;++d) {
                                const u32 attr=descDisk.readU32(), value=descDisk.readU32();
                                require(descriptors[d].mAttr==attr && descriptors[d].mType==value,"shape vertex descriptor matches disk");
                                if(attr==GX_VA_NULL) break;
                            }
                            for(u16 g=0;g<groups;++g) {
                                const auto& mtx=factory.mMtxInitData[firstMtx+g];
                                JSUMemoryInputStream mtxDisk(original+u32(b->mMatrixInitDataOffset)+(firstMtx+g)*8,8);
                                require(mtx.mUseMtxIndex==mtxDisk.readU16() && mtx.mUseMtxCount==mtxDisk.readU16() &&
                                        mtx.mFirstUseMtxIndex==mtxDisk.readU32(),"shape matrix group matches disk");
                                if(type==J3DShapeMtx_Multi) {
                                    JSUMemoryInputStream table(original+u32(b->mMatrixTableOffset)+mtx.mFirstUseMtxIndex*2,mtx.mUseMtxCount*2);
                                    for(u16 k=0;k<mtx.mUseMtxCount;++k)
                                        require(factory.mMtxTable[mtx.mFirstUseMtxIndex+k]==table.readU16(),"shape matrix palette matches disk");
                                }
                                const auto& draw=factory.mDrawInitData[firstDraw+g];
                                JSUMemoryInputStream drawDisk(original+u32(b->mMtxGroupTableOffset)+(firstDraw+g)*8,8);
                                require(draw.mDisplayListSize==drawDisk.readU32() && draw.mDisplayListIndex==drawDisk.readU32(),"shape draw packet matches disk");
                                require(!std::memcmp(factory.mDisplayListData+draw.mDisplayListIndex,
                                                    original+u32(b->mPrimDataOffset)+draw.mDisplayListIndex,draw.mDisplayListSize),"primitive commands retain exact big-endian bytes");
                                ++matrixGroups;
                            }
                            ++shapes;
                        }
                        require(!p2_load_shape(&factory,b,32),"truncated shape block rejected");
                        void* owned=factory.mNativeStorage;
                        p2::Bytes malformed(original,original+u32(b->mSize));
                        auto* bad=reinterpret_cast<J3DShapeBlock*>(malformed.data());
                        malformed[u32(b->mRemapTableOffset)]=0xff;
                        malformed[u32(b->mRemapTableOffset)+1]=0xff;
                        require(!p2_load_shape(&factory,bad,malformed.size()),"out-of-range shape remap rejected");
                        std::memcpy(malformed.data(),original,malformed.size());
                        bad->mPrimDataOffset=u32(b->mSize)+4;
                        require(!p2_load_shape(&factory,bad,malformed.size()),"out-of-range primitive buffer rejected");
                        std::memcpy(malformed.data(),original,malformed.size());
                        const auto& first=factory.mInitData[factory.mInitDataIndices[0]];
                        const size_t descriptor=u32(b->mAttribTableOffset)+first.mVtxDescListIndex;
                        std::memset(malformed.data()+descriptor,0xfe,4);
                        require(!p2_load_shape(&factory,bad,malformed.size()),"invalid vertex descriptor rejected");
                        if(first.mMtxGroupNum) {
                            std::memcpy(malformed.data(),original,malformed.size());
                            const size_t draw=u32(b->mMtxGroupTableOffset)+first.mShapeDrawInitDataIndex*8;
                            std::memset(malformed.data()+draw,0xff,4);
                            require(!p2_load_shape(&factory,bad,malformed.size()),"oversized primitive packet rejected");
                        }
                        require(factory.mNativeStorage==owned,"failed conversion preserves live shape storage");
                    }
                    require(heap->getTotalFreeSize()==freeBefore,"shape factory releases converted tables");
                    {
                        J3DShapeTable table;
                        u16 firstIndex=0;
                        {
                            J3DShapeFactory factory(*b);
                            firstIndex=factory.mInitDataIndices[0];
                            table.mNativeStorage=factory.mNativeStorage;
                            factory.mNativeStorage=nullptr;
                        }
                        const auto* native=static_cast<const J3DShapeBlock*>(table.mNativeStorage);
                        const auto* indices=JSUConvertOffsetToPtr<u16>(native,native->mRemapTableOffset);
                        const auto* init=JSUConvertOffsetToPtr<J3DShapeInitData>(native,native->mShapeDataOffset);
                        require(indices[0]==firstIndex && std::isfinite(init[indices[0]].mRadius),
                                "model retains native shape tables after factory destruction for billboard setup");
                    }
                    require(heap->getTotalFreeSize()==freeBefore,"model shape table releases transferred storage");
                    break;
                }
                case J3DFBT_Texture: {
                    auto* b = static_cast<const J3DTextureBlock*>(block);
                    offset(b,b->mTexHeaderOffset); names(b,b->mTexNameOffset,b->mTextureCount);
                    require(u32(b->mTexHeaderOffset)+u32(b->mTextureCount)*sizeof(ResTIMG)<=u32(b->mSize),"texture headers fit block");
                    auto* headers=JSUConvertOffsetToPtr<ResTIMG>(b,b->mTexHeaderOffset);
                    J3DTexture texture(b->mTextureCount,headers);
                    for(u16 n=0;n<texture.getNum();++n) {
                        const auto* tex=texture.getResTIMG(n);
                        JSUMemoryInputStream disk(tex,sizeof(*tex));
                        disk.skip(2);
                        require(tex->getWidth()==disk.readU16() && tex->getHeight()==disk.readU16(),"texture dimensions match disk");
                        require(tex->getWidth()>0 && tex->getWidth()<=1024 && tex->getHeight()>0 && tex->getHeight()<=1024,"texture dimensions supported by GX");
                        disk.skip(4);
                        require(tex->mPaletteEntryCount==disk.readU16() && tex->mPaletteOffset==disk.readU32(),"texture palette metadata matches disk");
                        disk.skip(10);
                        require(tex->mLODBias==disk.readS16() && tex->mImageDataOffset==disk.readU32(),"texture LOD and image offset match disk");
                        const auto* image=static_cast<const u8*>(p2_texture_image(tex,tex->mImageDataOffset));
                        require(image>=reinterpret_cast<const u8*>(b) && image<reinterpret_cast<const u8*>(b)+u32(b->mSize),"texture image starts inside TEX1");
                        const void* palette=p2_texture_palette(tex,tex->mPaletteOffset);
                        if(tex->mPaletteEntryCount) {
                            const auto* bytes=static_cast<const u8*>(palette);
                            require(bytes>=reinterpret_cast<const u8*>(b) && bytes+2*u32(tex->mPaletteEntryCount)<=reinterpret_cast<const u8*>(b)+u32(b->mSize),"texture palette fits TEX1");
                        }
                        ResTIMG copied{};
                        J3DTexture replacement(1,&copied);
                        replacement.changeImage(tex,0);
                        require(copied.getWidth()==tex->getWidth() && copied.getHeight()==tex->getHeight(),"original changeImage preserves converted dimensions");
                        require(p2_texture_image(&copied,copied.mImageDataOffset)==image &&
                                p2_texture_palette(&copied,copied.mPaletteOffset)==palette,"original changeImage preserves native resource addresses");
                        ++textures;
                    }
                    break;
                }
                }
                ++blocks;
            }
            ++models;
        }
        archive->unmount();
    }
    require(models >= 5, "real title scene models exercised");
    std::printf("Original MAT3 factory: %zu title materials.\n",materials);
    std::printf("Original J3D model data: %zu models, %zu blocks, %zu joints, %zu vertices, %zu skin influences, %zu hierarchy entries, %zu draw matrices, %zu textures, %zu shapes, %zu matrix groups.\n", models, blocks, joints, vertices, influences, hierarchyNodes, drawMatrices, textures, shapes, matrixGroups);
}
