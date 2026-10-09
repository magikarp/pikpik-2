#include "p2_animation.h"
#include "p2_game_alloc.h"
#include "p2_endian.h"
#include "JSystem/J3D/J3DAnmTransform.h"
#include "JSystem/J3D/J3DAnmTevRegKey.h"
#include "JSystem/J3D/J3DAnmTextureSRTKey.h"
#include "JSystem/J3D/J3DAnmColor.h"
#include "JSystem/J2D/J2DAnm.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace {
template<class T> T read(const unsigned char* p) { T value; std::memcpy(&value,p,sizeof(T)); return p2_big_endian(value); }
bool range(size_t at, size_t count, size_t stride, size_t size) { return at >= 0x24 && at <= size && count <= (size-at)/stride; }
}

bool p2_load_2d_transform(J2DAnmTransform* animation, const void* block, size_t size, bool key) {
    if(!animation) return false;
    J3DAnmTransformKey keyed;
    J3DAnmTransformFull full;
    J3DAnmTransform* converted=key ? static_cast<J3DAnmTransform*>(&keyed):static_cast<J3DAnmTransform*>(&full);
    if(!p2_load_transform(converted,block,size,key)) return false;
    p2_game_free(animation->mNativeStorage);
    animation->mNativeStorage=converted->mNativeStorage;
    converted->mNativeStorage=nullptr; // Transfer ownership; both evaluators use the same disk tables.
    animation->mScaleVals=converted->mScaleVals; animation->mRotationVals=converted->mRotationVals;
    animation->mTranslationVals=converted->mTranslationVals;
    animation->mTotalFrameCount=converted->mTotalFrameCount; animation->mAttribute=converted->mAttribute;
    animation->mCurrentFrame=0;
    if(key) {
        auto* out=static_cast<J2DAnmTransformKey*>(animation);
        out->_22=converted->mUpdateMaterialNum; out->mRotationScale=keyed._20; out->mInfoTable=keyed.mTable;
    } else {
        auto* out=static_cast<J2DAnmTransformFull*>(animation);
        out->_22=converted->mUpdateMaterialNum; out->mTable=full.mTable;
    }
    return true;
}

bool p2_load_tev_animation(J3DAnmTevRegKey* animation, const void* block, size_t size) {
    if (!animation || !block || size < 0x58) return false;
    const auto* b = static_cast<const unsigned char*>(block);
    const size_t declared = read<u32>(b+4);
    if (read<u32>(b) != 0x54524b31 || declared < 0x58 || read<s16>(b+10) < 0) return false;
    if (declared > size && (size > SIZE_MAX-31 || ((size+31)&~size_t(31)) != declared)) return false;
    if (declared < size) size=declared;
    const size_t materials[2]={read<u16>(b+12),read<u16>(b+14)};
    size_t tableAt[2], idsAt[2], namesAt[2], valuesAt[8], counts[8];
    auto bounded = [size](size_t at, size_t count, size_t stride) {
        return !count || (at >= 0x58 && !(at&1) && at <= size && count <= (size-at)/stride);
    };
    size_t bytes=(size+1)&~size_t(1);
    for (size_t channel=0;channel<8;++channel) {
        counts[channel]=read<u16>(b+16+channel*2);
        valuesAt[channel]=read<u32>(b+56+channel*4);
        if (!bounded(valuesAt[channel],counts[channel],2)) return false;
        bytes+=counts[channel]*2;
    }
    for (size_t group=0;group<2;++group) {
        tableAt[group]=read<u32>(b+32+group*4);
        idsAt[group]=read<u32>(b+40+group*4);
        namesAt[group]=read<u32>(b+48+group*4);
        const size_t n=materials[group];
        if (!bounded(tableAt[group],n,28) || !bounded(idsAt[group],n,2)) return false;
        bytes+=n*30;
        if (!n) continue;
        const size_t names=namesAt[group];
        if (!bounded(names,1,4) || read<u16>(b+names)!=n || n>(size-names-4)/4) return false;
        for (size_t i=0;i<n;++i) {
            const size_t off=read<u16>(b+names+6+i*4);
            if (off<4+n*4 || off>=size-names || !std::memchr(b+names+off,0,size-names-off)) return false;
            const auto* table=b+tableAt[group]+i*28;
            if (table[24]>3) return false; // TEV register selector
            for (size_t channel=0;channel<4;++channel) {
                const auto* track=table+channel*6;
                const size_t count=read<u16>(track), first=read<u16>(track+2), tangent=read<u16>(track+4);
                if (count>1 && tangent>1) return false;
                const size_t stride=tangent ? 4 : 3, needed=count>1 ? count*stride : count;
                const size_t available=counts[group*4+channel];
                if (needed && (first>available || needed>available-first)) return false;
                // Interpolation assumes ordered, distinct frame positions.
                for (size_t key=1;key<count;++key) {
                    const auto* values=b+valuesAt[group*4+channel]+first*2;
                    if (read<s16>(values+key*stride*2)<=read<s16>(values+(key-1)*stride*2)) return false;
                }
            }
        }
    }
    auto* native=static_cast<unsigned char*>(p2_game_alloc(bytes,16,nullptr));
    if (!native) return false;
    // Names retain their on-disc form. Separate decoded arrays also permit
    // aliased source tables without converting the same bytes twice.
    std::memcpy(native,b,size);
    size_t cursor=(size+1)&~size_t(1);
    s16* values[8]; void* tables[2]; u16* ids[2];
    for (size_t channel=0;channel<8;++channel) {
        values[channel]=reinterpret_cast<s16*>(native+cursor);
        for (size_t i=0;i<counts[channel];++i) values[channel][i]=read<s16>(b+valuesAt[channel]+i*2);
        cursor+=counts[channel]*2;
    }
    for (size_t group=0;group<2;++group) {
        tables[group]=native+cursor;
        for (size_t i=0;i<materials[group];++i) {
            auto* dest=native+cursor+i*28;
            const auto* source=b+tableAt[group]+i*28;
            for (size_t j=0;j<24;j+=2) { const u16 v=read<u16>(source+j); std::memcpy(dest+j,&v,2); }
            std::memcpy(dest+24,source+24,4);
        }
        cursor+=materials[group]*28;
        ids[group]=reinterpret_cast<u16*>(native+cursor);
        for (size_t i=0;i<materials[group];++i) ids[group][i]=read<u16>(b+idsAt[group]+i*2);
        cursor+=materials[group]*2;
    }
    p2_game_free(animation->mNativeStorage);
    animation->mNativeStorage=native;
    animation->mTotalFrameCount=read<s16>(b+10); animation->mAttribute=b[8]; animation->mCurrentFrame=0;
    animation->mCRegUpdateMaterialNum=materials[0]; animation->mKRegUpdateMaterialNum=materials[1];
    animation->mCRegKeyTable=static_cast<J3DAnmCRegKeyTable*>(tables[0]);
    animation->mKRegKeyTable=static_cast<J3DAnmKRegKeyTable*>(tables[1]);
    animation->mCRegUpdateMaterialID=ids[0]; animation->mKRegUpdateMaterialID=ids[1];
    animation->mCRegNameTable.setResource(materials[0] ? reinterpret_cast<ResNTAB*>(native+namesAt[0]) : nullptr);
    animation->mKRegNameTable.setResource(materials[1] ? reinterpret_cast<ResNTAB*>(native+namesAt[1]) : nullptr);
    animation->_10=counts[0]; animation->_12=counts[1]; animation->_14=counts[2]; animation->_16=counts[3];
    animation->_18=counts[4]; animation->_1A=counts[5]; animation->_1C=counts[6]; animation->_1E=counts[7];
    animation->mCRedVals=values[0]; animation->mCGreenVals=values[1]; animation->mCBlueVals=values[2]; animation->mCAlphaVals=values[3];
    animation->mKRedVals=values[4]; animation->mKGreenVals=values[5]; animation->mKBlueVals=values[6]; animation->mKAlphaVals=values[7];
    return true;
}
bool p2_load_transform(J3DAnmTransform* animation, const void* block, size_t size, bool key) {
    if (!animation || !block || size < 0x24) return false;
    const auto* b = static_cast<const unsigned char*>(block);
    const size_t declared = read<u32>(b+4);
    if (read<u32>(b) != (key ? 0x414e4b31u : 0x414e4631u) || declared < 0x24) return false;
    // Retail BCA resources can omit terminal 32-byte alignment padding.
    // All table/value ranges below still use the actual available bytes.
    if (declared > size && (size > SIZE_MAX-31 || ((size+31)&~size_t(31)) != declared)) return false;
    if (declared < size) size = declared;
    const size_t joints = read<u16>(b+12);
    const size_t tableAt = read<u32>(b+20), scaleAt = read<u32>(b+24), rotAt = read<u32>(b+28), transAt = read<u32>(b+32);
    const size_t counts[3] = {read<u16>(b+14), read<u16>(b+16), read<u16>(b+18)};
    const size_t tableStride = key ? sizeof(J3DAnmTransformKeyTable) : sizeof(J3DAnmTransformFullTable);
    if (!joints || (key && b[9] > 15) || read<s16>(b+10) < 0 || !range(tableAt,joints*3,tableStride,size) ||
        !range(scaleAt,counts[0],4,size) || !range(rotAt,counts[1],2,size) || !range(transAt,counts[2],4,size)) return false;
    // Validate every channel before letting the original evaluator follow indices.
    for (size_t axis=0; axis<joints*3; ++axis) {
        for (size_t channel=0; channel<3; ++channel) {
            const auto* track = b+tableAt+axis*tableStride+channel*(key ? 6 : 4);
            const size_t count = read<u16>(track), first = read<u16>(track+2);
            size_t needed = count;
            if (key && count > 1) {
                const u16 tangent = read<u16>(track+4);
                if (tangent > 1) return false;
                needed *= tangent ? 4 : 3;
            }
            if (needed && (first > counts[channel] || needed > counts[channel]-first)) return false;
            if (!key && !count) return false; // full sampler always accesses a value
        }
    }
    const size_t tableBytes = joints*3*tableStride;
    const size_t scaleStart = (tableBytes+3)&~size_t(3);
    const size_t rotStart = scaleStart+counts[0]*4;
    const size_t transStart = (rotStart+counts[1]*2+3)&~size_t(3);
    auto* native = static_cast<unsigned char*>(p2_game_alloc(transStart+counts[2]*4, 16, nullptr));
    if (!native) return false;
    for (size_t i=0; i<tableBytes; i+=2) {
        u16 value = read<u16>(b+tableAt+i); std::memcpy(native+i,&value,2);
    }
    auto* scales = reinterpret_cast<f32*>(native+scaleStart);
    auto* rotations = reinterpret_cast<s16*>(native+rotStart);
    auto* translations = reinterpret_cast<f32*>(native+transStart);
    for (size_t i=0;i<counts[0];++i) scales[i]=read<f32>(b+scaleAt+i*4);
    for (size_t i=0;i<counts[1];++i) rotations[i]=read<s16>(b+rotAt+i*2);
    for (size_t i=0;i<counts[2];++i) translations[i]=read<f32>(b+transAt+i*4);
    p2_game_free(animation->mNativeStorage);
    animation->mNativeStorage=native;
    animation->mScaleVals=scales; animation->mRotationVals=rotations; animation->mTranslationVals=translations;
    animation->mUpdateMaterialNum=joints; animation->mTotalFrameCount=read<s16>(b+10);
    animation->mAttribute=b[8]; animation->mCurrentFrame=0;
    if (key) {
        auto* result=static_cast<J3DAnmTransformKey*>(animation);
        result->_20=b[9]; result->mTable=reinterpret_cast<J3DAnmTransformKeyTable*>(native);
    } else static_cast<J3DAnmTransformFull*>(animation)->mTable=reinterpret_cast<J3DAnmTransformFullTable*>(native);
    return true;
}


bool p2_load_texture_animation(J3DAnmTextureSRTKey* animation, const void* block, size_t size) {
    if (!animation || !block || size<0x60) return false;
    const auto* b=static_cast<const unsigned char*>(block);
    const size_t declared=read<u32>(b+4);
    if (read<u32>(b)!=0x54544b31 || declared<0x60 || read<s16>(b+10)<0 || b[9]>15) return false;
    if (declared>size && (size>SIZE_MAX-31 || ((size+31)&~size_t(31))!=declared)) return false;
    if (declared<size) size=declared;
    struct Group {
        size_t tracks, counts[3], at[7], names, dest[7];
    } groups[2]{};
    // Arrays: key tables, material IDs, texture-matrix IDs, centers, S/R/T values.
    const size_t strides[]={18,2,1,12,4,2,4};
    size_t bytes=(size+3)&~size_t(3);
    auto bounded=[size](size_t at,size_t count,size_t stride) {
        return !count || (at>=0x60 && at<=size && count<=(size-at)/stride);
    };
    for (int index=0;index<2;++index) {
        auto& g=groups[index];
        const size_t h=index ? 0x34:0x0c, offsets=index ? 0x3c:0x14;
        g.tracks=read<u16>(b+h);
        if (g.tracks%3) return false;
        for(int channel=0;channel<3;++channel) g.counts[channel]=read<u16>(b+h+2+channel*2);
        g.at[0]=read<u32>(b+offsets); g.at[1]=read<u32>(b+offsets+4);
        g.names=read<u32>(b+offsets+8);
        for(int a=2;a<7;++a) g.at[a]=read<u32>(b+offsets+4+a*4);
        const size_t materials=g.tracks/3;
        const size_t lengths[]={g.tracks,materials,materials,materials,g.counts[0],g.counts[1],g.counts[2]};
        for(int a=0;a<7;++a) {
            if (!bounded(g.at[a],lengths[a],strides[a])) return false;
            g.dest[a]=bytes; bytes=(bytes+lengths[a]*strides[a]+3)&~size_t(3);
        }
        if(materials) {
            if (!bounded(g.names,1,4) || (g.names&1) || read<u16>(b+g.names)!=materials || materials>(size-g.names-4)/4) return false;
            for(size_t i=0;i<materials;++i) {
                const size_t off=read<u16>(b+g.names+6+i*4);
                if(off<4+materials*4 || off>=size-g.names || !std::memchr(b+g.names+off,0,size-g.names-off)) return false;
            }
            for(size_t i=0;i<materials*3;++i) if(!std::isfinite(read<f32>(b+g.at[3]+i*4))) return false;
        }
        for(int channel=0;channel<3;++channel) {
            if(channel!=1) for(size_t i=0;i<g.counts[channel];++i)
                if(!std::isfinite(read<f32>(b+g.at[4+channel]+i*4))) return false;
            for(size_t axis=0;axis<g.tracks;++axis) {
                const auto* track=b+g.at[0]+axis*18+channel*6;
                const size_t count=read<u16>(track), first=read<u16>(track+2), tangent=read<u16>(track+4);
                if(count>1 && tangent>1) return false;
                const size_t stride=tangent ? 4:3, needed=count>1 ? count*stride:count;
                if(needed && (first>g.counts[channel] || needed>g.counts[channel]-first)) return false;
                for(size_t key=1;key<count;++key) {
                    const size_t a=first+(key-1)*stride, c=first+key*stride;
                    const auto* values=b+g.at[4+channel];
                    if(channel==1 ? read<s16>(values+c*2)<=read<s16>(values+a*2)
                                  : read<f32>(values+c*4)<=read<f32>(values+a*4)) return false;
                }
            }
        }
    }
    auto* native=static_cast<unsigned char*>(p2_game_alloc(bytes,16,nullptr));
    if(!native) return false;
    std::memcpy(native,b,size); // Name tables remain in their on-disc representation.
    for(auto& g:groups) {
        const size_t lengths[]={g.tracks*9,g.tracks/3,g.tracks/3,g.tracks,g.counts[0],g.counts[1],g.counts[2]};
        for(int a=0;a<7;++a) {
            for(size_t i=0;i<lengths[a];++i) {
                if(a==2) native[g.dest[a]+i]=b[g.at[a]+i];
                else if(a==0 || a==1 || a==5) {
                    const u16 v=read<u16>(b+g.at[a]+i*2); std::memcpy(native+g.dest[a]+i*2,&v,2);
                } else {
                    const f32 v=read<f32>(b+g.at[a]+i*4); std::memcpy(native+g.dest[a]+i*4,&v,4);
                }
            }
        }
    }
    p2_game_free(animation->mNativeStorage);
    animation->mNativeStorage=native;
    animation->mAttribute=b[8]; animation->mRotationScale=b[9];
    animation->mTotalFrameCount=read<s16>(b+10); animation->mCurrentFrame=0;
    const u32 calc=read<u32>(b+0x5c); animation->mTexMtxCalcType=calc<=1 ? calc:0;
    const auto& g=groups[0]; const auto& p=groups[1];
    animation->mTrackNum=g.tracks; animation->mScaleNum=g.counts[0]; animation->mRotNum=g.counts[1]; animation->mTransNum=g.counts[2];
    animation->mTable1=reinterpret_cast<J3DAnmTransformKeyTable*>(native+g.dest[0]);
    animation->mUpdateMaterialID=reinterpret_cast<u16*>(native+g.dest[1]);
    animation->mUpdateTexMtxID=native+g.dest[2]; animation->mSRTCenter=reinterpret_cast<Vec*>(native+g.dest[3]);
    animation->mScale1Vals=reinterpret_cast<f32*>(native+g.dest[4]);
    animation->mRotation1Vals=reinterpret_cast<s16*>(native+g.dest[5]);
    animation->mTranslation1Vals=reinterpret_cast<f32*>(native+g.dest[6]);
    animation->mUpdateMaterialName.setResource(g.tracks ? reinterpret_cast<ResNTAB*>(native+g.names):nullptr);
    animation->mPostTrackNum=p.tracks; animation->_44=p.counts[0]; animation->_46=p.counts[1]; animation->_48=p.counts[2];
    animation->mTransformKeyTable=reinterpret_cast<J3DAnmTransformKeyTable*>(native+p.dest[0]);
    animation->mPostUpdateMaterialID=reinterpret_cast<u16*>(native+p.dest[1]);
    animation->mPostUpdateTexMtxID=native+p.dest[2]; animation->mPostSRTCenter=reinterpret_cast<Vec*>(native+p.dest[3]);
    animation->_4C=reinterpret_cast<f32*>(native+p.dest[4]); animation->_50=reinterpret_cast<s16*>(native+p.dest[5]);
    animation->_54=reinterpret_cast<f32*>(native+p.dest[6]);
    animation->mPostUpdateMaterialName.setResource(p.tracks ? reinterpret_cast<ResNTAB*>(native+p.names):nullptr);
    return true;
}

bool p2_load_2d_texture_srt(J2DAnmTextureSRTKey* animation,const void* block,size_t size) {
    if(!animation) return false;
    J3DAnmTextureSRTKey converted;
    if(!p2_load_texture_animation(&converted,block,size)) return false;
    p2_game_free(animation->mNativeStorage);
    animation->mNativeStorage=converted.mNativeStorage;
    converted.mNativeStorage=nullptr; // Transfer ownership; both evaluators use the same tables.
    // Field mapping follows the original J2DAnmKeyLoader_v15::setAnmTextureSRT.
    animation->mTotalFrameCount=converted.mTotalFrameCount; animation->mAttribute=converted.mAttribute;
    animation->mCurrentFrame=0; animation->mRotationScale=converted.mRotationScale;
    animation->mUpdateMaterialNum=converted.mTrackNum; animation->_1C=converted.mScaleNum;
    animation->_1E=converted.mRotNum; animation->_20=converted.mTransNum;
    animation->mInfoTable=converted.mTable1; animation->mUpdateMaterialID=converted.mUpdateMaterialID;
    animation->mNameTab=converted.mUpdateMaterialName; animation->mUpdateTexMtxID=converted.mUpdateTexMtxID;
    animation->_48=converted.mSRTCenter; animation->mScaleVals=converted.mScale1Vals;
    animation->mRotationVals=converted.mRotation1Vals; animation->mTranslationVals=converted.mTranslation1Vals;
    animation->mNameTab2=converted.mPostUpdateMaterialName;
    animation->_60=converted.mPostTrackNum; animation->_4C=converted._44; animation->_4E=converted._46; animation->_50=converted._48;
    animation->_64=converted.mTransformKeyTable; animation->_6C=converted.mPostUpdateMaterialID;
    animation->_68=converted.mPostUpdateTexMtxID; animation->_80=converted.mPostSRTCenter;
    animation->_54=converted._4C; animation->_58=converted._50; animation->_5C=converted._54;
    animation->_84=converted.mTexMtxCalcType==1 ? 1 : 0;
    return true;
}

bool p2_load_2d_tev_reg(J2DAnmTevRegKey* animation,const void* block,size_t size) {
    if(!animation) return false;
    J3DAnmTevRegKey converted;
    if(!p2_load_tev_animation(&converted,block,size)) return false;
    p2_game_free(animation->mNativeStorage);
    animation->mNativeStorage=converted.mNativeStorage;
    converted.mNativeStorage=nullptr; // Transfer ownership; both evaluators use the same tables.
    // Field mapping follows the original J2DAnmKeyLoader_v15::setAnmTevReg.
    animation->mTotalFrameCount=converted.mTotalFrameCount; animation->mAttribute=converted.mAttribute;
    animation->mCurrentFrame=0;
    animation->mCRegUpdateMaterialNum=converted.mCRegUpdateMaterialNum; animation->mKRegUpdateMaterialNum=converted.mKRegUpdateMaterialNum;
    animation->_14=converted._10; animation->_16=converted._12; animation->_18=converted._14; animation->_1A=converted._16;
    animation->_1C=converted._18; animation->_1E=converted._1A; animation->_20=converted._1C; animation->_22=converted._1E;
    animation->mCRegUpdateMaterialID=converted.mCRegUpdateMaterialID; animation->mCRegNameTab=converted.mCRegNameTable;
    animation->mKRegUpdateMaterialID=converted.mKRegUpdateMaterialID; animation->mKRegNameTab=converted.mKRegNameTable;
    animation->mCRegKeyTable=converted.mCRegKeyTable; animation->mKRegKeyTable=converted.mKRegKeyTable;
    animation->mCRedVals=converted.mCRedVals; animation->mCGreenVals=converted.mCGreenVals;
    animation->mCBlueVals=converted.mCBlueVals; animation->mCAlphaVals=converted.mCAlphaVals;
    animation->mKRedVals=converted.mKRedVals; animation->mKGreenVals=converted.mKGreenVals;
    animation->mKBlueVals=converted.mKBlueVals; animation->mKAlphaVals=converted.mKAlphaVals;
    return true;
}

bool p2_load_color_key(J3DAnmColorKey* animation,const void* block,size_t size) {
    if(!animation) return false;
    J2DAnmColorKey parsed; // Same PAK1 layout; reuse the validated 2D conversion.
    if(!p2_load_2d_color(&parsed,block,size)) return false;
    p2_game_free(animation->mNativeStorage);
    animation->mNativeStorage=parsed.mNativeStorage;
    parsed.mNativeStorage=nullptr; // Transfer ownership.
    // Field mapping follows the original J3DAnmKeyLoader_v15::setAnmColor.
    animation->mTotalFrameCount=parsed.mTotalFrameCount; animation->mAttribute=parsed.mAttribute;
    animation->mCurrentFrame=0.0f; animation->mUpdateMaterialNum=parsed.mUpdateMaterialNum;
    animation->_0C=parsed._10; animation->_0E=parsed._12; animation->_10=parsed._14; animation->_12=parsed._16;
    animation->mTable=parsed.mTables;
    animation->mRedValue=parsed.mRedVals; animation->mGreenValue=parsed.mGreenVals;
    animation->mBlueValue=parsed.mBlueVals; animation->mAlphaValue=parsed.mAlphaVals;
    animation->mUpdateMaterialID=parsed.mUpdateMaterialID; animation->mNameTab=parsed.mNameTab;
    return true;
}

bool p2_load_2d_color(J2DAnmColorKey* animation,const void* block,size_t size) {
    if(!animation || !block || size<0x34) return false;
    const auto* b=static_cast<const unsigned char*>(block);
    const size_t declared=read<u32>(b+4);
    if(read<u32>(b)!=0x50414b31 || declared<0x34 || read<s16>(b+12)<0) return false;
    if(declared>size && (size>SIZE_MAX-31 || ((size+31)&~size_t(31))!=declared)) return false;
    if(declared<size) size=declared;
    const size_t n=read<u16>(b+14), table=read<u32>(b+24), ids=read<u32>(b+28), names=read<u32>(b+32);
    auto bounded=[size](size_t at,size_t count,size_t stride) {
        return !count || (at>=0x34 && !(at&1) && at<=size && count<=(size-at)/stride);
    };
    if(!bounded(table,n,24) || !bounded(ids,n,2)) return false;
    if(n) {
        if(!bounded(names,1,4) || read<u16>(b+names)!=n || n>(size-names-4)/4) return false;
        for(size_t i=0;i<n;++i) {
            const size_t off=read<u16>(b+names+6+i*4);
            if(off<4+n*4 || off>=size-names || !std::memchr(b+names+off,0,size-names-off)) return false;
        }
    }
    size_t counts[4], at[4], bytes=((size+1)&~size_t(1))+n*26;
    for(size_t channel=0;channel<4;++channel) {
        counts[channel]=read<u16>(b+16+channel*2); at[channel]=read<u32>(b+36+channel*4);
        if(!bounded(at[channel],counts[channel],2)) return false;
        bytes+=counts[channel]*2;
        for(size_t i=0;i<n;++i) {
            const auto* track=b+table+i*24+channel*6;
            const size_t count=read<u16>(track), first=read<u16>(track+2), tangent=read<u16>(track+4);
            if(count>1 && tangent>1) return false;
            const size_t stride=tangent ? 4:3, needed=count>1 ? count*stride:count;
            if(needed && (first>counts[channel] || needed>counts[channel]-first)) return false;
            for(size_t key=1;key<count;++key) {
                const auto* values=b+at[channel]+first*2;
                // Retail color tracks repeat frame times to encode abrupt changes.
                if(read<s16>(values+key*stride*2)<read<s16>(values+(key-1)*stride*2)) return false;
            }
        }
    }
    auto* native=static_cast<unsigned char*>(p2_game_alloc(bytes,16,nullptr));
    if(!native) return false;
    std::memcpy(native,b,size);
    size_t cursor=(size+1)&~size_t(1);
    auto* tables=reinterpret_cast<J3DAnmColorKeyTable*>(native+cursor);
    for(size_t i=0;i<n*12;++i) { const u16 v=read<u16>(b+table+i*2); std::memcpy(native+cursor+i*2,&v,2); }
    cursor+=n*24;
    auto* materialIDs=reinterpret_cast<u16*>(native+cursor);
    for(size_t i=0;i<n;++i) materialIDs[i]=read<u16>(b+ids+i*2);
    cursor+=n*2;
    s16* values[4];
    for(size_t channel=0;channel<4;++channel) {
        values[channel]=reinterpret_cast<s16*>(native+cursor);
        for(size_t i=0;i<counts[channel];++i) values[channel][i]=read<s16>(b+at[channel]+i*2);
        cursor+=counts[channel]*2;
    }
    p2_game_free(animation->mNativeStorage); animation->mNativeStorage=native;
    animation->mTables=tables; animation->mUpdateMaterialID=materialIDs; animation->mUpdateMaterialNum=n;
    animation->mNameTab.setResource(n ? reinterpret_cast<ResNTAB*>(native+names):nullptr);
    animation->_10=counts[0]; animation->_12=counts[1]; animation->_14=counts[2]; animation->_16=counts[3];
    animation->mRedVals=values[0]; animation->mGreenVals=values[1]; animation->mBlueVals=values[2]; animation->mAlphaVals=values[3];
    animation->mAttribute=b[8]; animation->mTotalFrameCount=read<s16>(b+12); animation->mCurrentFrame=0;
    return true;
}

bool p2_load_2d_pattern(J2DAnmTexPattern* animation,const void* block,size_t size) {
    if(!animation || !block || size<0x20) return false;
    const auto* b=static_cast<const unsigned char*>(block);
    const size_t declared=read<u32>(b+4);
    if(read<u32>(b)!=0x54505431 || declared<0x20 || declared>size || read<s16>(b+10)<0) return false;
    size=declared;
    const size_t n=read<u16>(b+12), values=read<u16>(b+14);
    const size_t table=read<u32>(b+16), at=read<u32>(b+20), ids=read<u32>(b+24), names=read<u32>(b+28);
    auto bounded=[size](size_t offset,size_t count,size_t stride) {
        return !count || (offset>=0x20 && !(offset&1) && offset<=size && count<=(size-offset)/stride);
    };
    if(!bounded(table,n,8) || !bounded(at,values,2) || !bounded(ids,n,2)) return false;
    if(n) {
        if(!bounded(names,1,4) || read<u16>(b+names)!=n || n>(size-names-4)/4) return false;
        for(size_t i=0;i<n;++i) {
            const size_t off=read<u16>(b+names+6+i*4);
            if(off<4+n*4 || off>=size-names || !std::memchr(b+names+off,0,size-names-off)) return false;
            const size_t count=read<u16>(b+table+i*8), first=read<u16>(b+table+i*8+2);
            if(!count || first>values || count>values-first) return false;
        }
    }
    const size_t aligned=(size+1)&~size_t(1);
    auto* native=static_cast<unsigned char*>(p2_game_alloc(aligned+n*10+values*2,16,nullptr));
    if(!native) return false;
    std::memcpy(native,b,size);
    auto* tables=reinterpret_cast<u16*>(native+aligned);
    auto* bindings=tables+n*4;
    auto* samples=bindings+n;
    for(size_t i=0;i<n*4;++i) tables[i]=read<u16>(b+table+i*2);
    for(size_t i=0;i<n;++i) bindings[i]=read<u16>(b+ids+i*2);
    for(size_t i=0;i<values;++i) samples[i]=read<u16>(b+at+i*2);
    p2_game_free(animation->mNativeStorage);
    delete[] animation->mImgPtrArray; animation->mImgPtrArray=nullptr;
    animation->mNativeStorage=native;
    animation->mTotalFrameCount=read<s16>(b+10); animation->mAttribute=b[8]; animation->mCurrentFrame=0;
    animation->mUpdateMaterialNum=n; animation->_18=values;
    animation->mAnmTable=reinterpret_cast<J3DAnmTexPatternFullTable*>(tables);
    animation->mUpdateMaterialID=bindings; animation->mValues=samples;
    animation->mNameTab.setResource(n ? reinterpret_cast<ResNTAB*>(native+names):nullptr);
    return true;
}

// J2D full visibility (VAF1, used by the Challenge result screens). The table
// of (frame count, first value) u16 pairs is converted; the u8 values are
// endian-neutral and stay in the copied block.
bool p2_load_2d_visibility(J2DAnmVisibilityFull* animation,const void* block,size_t size) {
    if(!animation || !block || size<0x18) return false;
    const auto* b=static_cast<const unsigned char*>(block);
    const size_t declared=read<u32>(b+4);
    if(read<u32>(b)!=0x56414631 || declared<0x18 || declared>size || read<s16>(b+10)<0) return false;
    size=declared;
    const size_t n=read<u16>(b+12), values=read<u16>(b+14);
    const size_t table=read<u32>(b+16), at=read<u32>(b+20);
    if(n && (table<0x18 || table>size || n>(size-table)/4)) return false;
    if(values && (at<0x18 || at>size || values>size-at)) return false;
    for(size_t i=0;i<n;++i) {
        const size_t count=read<u16>(b+table+i*4), first=read<u16>(b+table+i*4+2);
        if(!count || first>values || count>values-first) return false;
    }
    const size_t aligned=(size+1)&~size_t(1);
    auto* native=static_cast<unsigned char*>(p2_game_alloc(aligned+n*4,16,nullptr));
    if(!native) return false;
    std::memcpy(native,b,size);
    auto* tables=reinterpret_cast<u16*>(native+aligned);
    for(size_t i=0;i<n*2;++i) tables[i]=read<u16>(b+table+i*2);
    p2_game_free(animation->mNativeStorage);
    animation->mNativeStorage=native;
    animation->mTotalFrameCount=read<s16>(b+10); animation->mAttribute=b[8]; animation->mCurrentFrame=0;
    animation->mAnimTableNum1=u16(n); animation->mAnimTableNum2=u16(values);
    animation->mTable=reinterpret_cast<J3DAnmVisibilityFullTable*>(tables);
    animation->mValues=native+at;
    return true;
}
