// Aurora exposes portable C matrix functions; the decomp calls the paired-single
// symbols directly. Keep SDK headers isolated in this translation unit.
#include <dolphin/mtx.h>
#include <cstdint>
#include <cstring>
static_assert(sizeof(u32)==4,"native SDK words must stay 32-bit");
#undef PSMTXIdentity
#undef PSMTXCopy
#undef PSMTXConcat
#undef PSMTXTranspose
#undef PSMTXInverse
#undef PSMTXRotRad
#undef PSMTXRotTrig
#undef PSMTXTrans
#undef PSMTXTransApply
#undef PSMTXScale
#undef PSMTXScaleApply
#undef PSMTXQuat
#undef PSMTXMultVec
#undef PSMTXMultVecArraySR
#undef PSVECAdd
#undef PSVECSubtract
#undef PSVECCrossProduct
extern "C" {
void PSMTXIdentity(Mtx m) { C_MTXIdentity(m); }
void PSMTXCopy(const Mtx a, Mtx b) { C_MTXCopy(a,b); }
void PSMTXConcat(const Mtx a, const Mtx b, Mtx c) { C_MTXConcat(a,b,c); }
void PSMTXTranspose(const Mtx a, Mtx b) { C_MTXTranspose(a,b); }
u32 PSMTXInverse(const Mtx a, Mtx b) { return C_MTXInverse(a,b); }
void PSMTXRotRad(Mtx m, char axis, float angle) { C_MTXRotRad(m,axis,angle); }
void PSMTXRotTrig(Mtx m, char axis, float s, float c) { C_MTXRotTrig(m,axis,s,c); }
void PSMTXTrans(Mtx m, float x, float y, float z) { C_MTXTrans(m,x,y,z); }
void PSMTXTransApply(const Mtx a, Mtx b, float x, float y, float z) { C_MTXTransApply(a,b,x,y,z); }
void PSMTXScale(Mtx m, float x, float y, float z) { C_MTXScale(m,x,y,z); }
void PSMTXScaleApply(const Mtx a, Mtx b, float x, float y, float z) { C_MTXScaleApply(a,b,x,y,z); }
void PSMTXQuat(Mtx m, const Quaternion* q) { C_MTXQuat(m,q); }
void PSMTXMultVec(const Mtx m, const Vec* in, Vec* out) { C_MTXMultVec(m,in,out); }
// The decomp declares count as float* and its three particle callers pass
// (float*)2. Preserve that existing ABI until the source declaration is repaired.
void PSMTXMultVecArraySR(const Mtx m, float* in, float* out, float* count) {
    C_MTXMultVecArraySR(m,reinterpret_cast<const Vec*>(in),reinterpret_cast<Vec*>(out),
                       static_cast<u32>(reinterpret_cast<uintptr_t>(count)));
}
void PSMTX44Copy(float src[4][4], float dst[4][4]) { std::memmove(dst,src,16*sizeof(float)); }
void PSVECAdd(const Vec* a,const Vec* b,Vec* c) { C_VECAdd(a,b,c); }
void PSVECSubtract(const Vec* a,const Vec* b,Vec* c) { C_VECSubtract(a,b,c); }
void PSVECCrossProduct(const Vec* a,const Vec* b,Vec* c) { C_VECCrossProduct(a,b,c); }
}

// Original implementation is Metrowerks assembly only. J3D passes the first
// matrix of contiguous envelope arrays; retain the common left-hand transform.
void J3DPSMtxArrayConcat(float (*a)[4], float (*b)[4], float (*out)[4], u32 count) {
    Mtx left;
    C_MTXCopy(a,left);
    for(u32 i=0;i<count;++i) C_MTXConcat(left,b+3*i,out+3*i);
}
