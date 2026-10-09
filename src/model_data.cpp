#include "p2_model_data.h"
#include "p2_endian.h"
#include "JSystem/J3D/J3DJointFactory.h"
#include <cstring>

namespace {
template<class T> T read(const unsigned char* p) { T value; std::memcpy(&value,p,sizeof(T)); return p2_big_endian(value); }
}
J3DJointInitData p2_decode_joint(const J3DJointInitData* source) {
    static_assert(sizeof(J3DJointInitData)==64,"JNT1 initialization record width");
    const auto* b=reinterpret_cast<const unsigned char*>(source);
    J3DJointInitData result{};
    result.mKind=read<u16>(b);
    result.mIgnoreParentScaling=static_cast<s8>(b[2]);
    result.mTransformInfo.mScale={read<f32>(b+4),read<f32>(b+8),read<f32>(b+12)};
    result.mTransformInfo.mRotation={read<s16>(b+16),read<s16>(b+18),read<s16>(b+20)};
    result.mTransformInfo.mTranslation={read<f32>(b+24),read<f32>(b+28),read<f32>(b+32)};
    result.mRadius=read<f32>(b+36);
    result.mMin.set(read<f32>(b+40),read<f32>(b+44),read<f32>(b+48));
    result.mMax.set(read<f32>(b+52),read<f32>(b+56),read<f32>(b+60));
    return result;
}
