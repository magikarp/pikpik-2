#include "Dolphin/mtx.h"
#include <cmath>
#include <cstdio>
void J3DPSMtxArrayConcat(float (*)[4],float (*)[4],float (*)[4],u32);
static int failures;
static void check(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",what); ++failures; } }
static bool near(float a,float b) { return std::fabs(a-b)<0.0001f; }
int main() {
    Mtx a,b,c;
    PSMTXTrans(a,10,20,30); PSMTXScale(b,2,3,4); PSMTXConcat(a,b,c);
    Vec v={1,2,3}; PSMTXMultVec(c,&v,&v);
    check(near(v.x,12)&&near(v.y,26)&&near(v.z,42),"composed affine transform in place");
    check(PSMTXInverse(c,b)==1,"invertible matrix"); PSMTXMultVec(b,&v,&v);
    check(near(v.x,1)&&near(v.y,2)&&near(v.z,3),"inverse recovers input");
    Vec pair[2]={{1,2,3},{-1,0,2}};
    PSMTXMultVecArraySR(c,(float*)pair,(float*)pair,(float*)2);
    check(near(pair[0].x,2)&&near(pair[0].y,6)&&near(pair[0].z,12)&&near(pair[1].x,-2)&&near(pair[1].z,8),"particle count ABI and in-place array rotation/scale");
    PSMTXRotRad(a,'z',1.57079632679f); v={1,0,0}; PSMTXMultVec(a,&v,&v);
    check(near(v.x,0)&&near(v.y,1),"axis rotation");
    PSQuaternion q={0,0,0,1}; PSMTXQuat(a,&q); v={2,3,4}; PSMTXMultVec(a,&v,&v);
    check(near(v.x,2)&&near(v.y,3)&&near(v.z,4),"quaternion ABI");
    Vec x={1,0,0},y={0,1,0}; PSVECCrossProduct(&x,&y,&x);
    check(near(x.x,0)&&near(x.y,0)&&near(x.z,1),"aliased cross product");
    Mtx44 from={},to={}; for(int i=0;i<4;++i)for(int j=0;j<4;++j)from[i][j]=float(i*4+j);
    PSMTX44Copy(from,to); check(to[3][3]==15&&to[2][1]==9,"full 4x4 copy");
    Mtx envelopes[2]; PSMTXTrans(a,10,20,30);
    PSMTXTrans(envelopes[0],1,2,3); PSMTXTrans(envelopes[1],4,5,6);
    J3DPSMtxArrayConcat(a,envelopes[0],envelopes[0],2);
    check(envelopes[0][0][3]==11&&envelopes[1][2][3]==36,"J3D envelope array stride and in-place concatenation");
    J3DPSMtxArrayConcat(a,envelopes[0],envelopes[0],1);
    check(envelopes[0][0][3]==21&&envelopes[1][2][3]==36,"single J3D envelope leaves next matrix untouched");
    J3DPSMtxArrayConcat(a,envelopes[0],envelopes[0],0);
    check(envelopes[0][0][3]==21,"empty J3D envelope array");
    std::printf("Renderer matrix ABI checks: %s\n",failures?"FAILED":"passed"); return failures?1:0;
}
