#include "JSystem/J3D/J3DMtxCalc.h"
#include "JSystem/J3D/J3DAnmLoader.h"
#include "JSystem/J3D/J3DMtxBuffer.h"
#include "JSystem/J3D/J3DJointTree.h"
#include "JSystem/J3D/J3DDrawBuffer.h"
#include "JSystem/JMath.h"
#include "p2_memory.h"
#include "JSystem/JUtility/TColor.h"
#include "JSystem/J2D/J2DGXColorS10.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void require(bool ok,const char* why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
static bool near(float a,float b) { return std::fabs(a-b)<0.001f; }
struct CameraAnimation : J3DAnmTransform {
    void getTransform(u16,J3DTransformInfo* t) const override {
        *t={}; t->mScale={1,1,1}; t->mTranslation={-6.88f,1916.77f,-40.4341f};
    }
};
int main() {
    require(p2_memory_init(128*1024*1024),"memory");
    auto* heap=JKRExpHeap::createRoot(16,false); require(heap,"heap");
    JUtility::TColor color(0x12345678u);
    require(color.r==0x12 && color.g==0x34 && color.b==0x56 && color.a==0x78,"packed RGBA decodes numeric channel order");
    color.a=0;
    require(color.toUInt32()==0x12345600u,"transparent color stays transparent on FIFO");
    color.set(10,20,30,128);
    require(color.toUInt32()==0x0a141e80u,"component colors encode FIFO RGBA");
    J2DGXColorS10 sourceColor(10,20,30,0), targetColor(99,99,99,255);
    targetColor=sourceColor;
    require(targetColor.r==10 && targetColor.g==20 && targetColor.b==30 && targetColor.a==0,"J2D material assignment updates actual color and transparency");
    const u64 packed=0xfff60014001e0080ULL;
    J2DGXColorS10 packedColor(packed);
    require(packedColor.r==-10 && packedColor.g==20 && packedColor.b==30 && packedColor.a==128 && packedColor.toUInt64()==packed,"J2D signed packed color channel order");
    Mtx base={{1,0,0,10},{0,1,0,20},{0,0,1,30}}, scaled;
    JMAMTXApplyScale(base,scaled,2,3,4);
    require(scaled[0][0]==2 && scaled[1][1]==3 && scaled[2][2]==4 && scaled[0][3]==10 && scaled[2][3]==30,"column scaling preserves translation");
    JMAMTXApplyScale(scaled,scaled,0.5f,1.0f/3,0.25f);
    require(scaled[0][0]==1 && scaled[1][1]==1 && scaled[2][2]==1 && scaled[1][3]==20,"in-place scaling");
    J3DMtxBuffer buffer; Mtx world[1]={}; u8 flags[1]={};
    buffer.mWorldMatrices=world; buffer.mScaleFlags=flags;
    J3DJoint joint; joint.mJointIdx=0;
    J3DMtxCalc::setMtxBuffer(&buffer); J3DMtxCalc::setJoint(&joint);
    CameraAnimation animation;
    for(unsigned type=0;type<3;++type) {
        auto* calc=J3DNewMtxCalcAnm(type,&animation);
        Vec unit={1,1,1}; calc->init(unit,base); calc->calc();
        require(near(world[0][0][3],3.12f) && near(world[0][1][3],1936.77f) && near(world[0][2][3],-10.4341f),"animated joint world translation");
        require(near(world[0][0][0],1) && near(world[0][1][1],1) && near(world[0][2][2],1),"animated joint basis");
        delete calc;
    }
    J3DJointTree tree;
    u8 counts[]={2,1}, jointScale[]={1,0}, envelopeScale[]={9,9};
    u16 indices[]={0,1,0}; f32 weights[]={0.25f,0.75f,1.0f};
    Mtx joints[]={{{1,0,0,10},{0,1,0,20},{0,0,1,30}},
                  {{2,0,0,30},{0,2,0,40},{0,0,2,50}}};
    Mtx inverse[]={{{1,0,0,-2},{0,1,0,-3},{0,0,1,-4}},
                   {{1,0,0,-5},{0,1,0,-6},{0,0,1,-7}}};
    Mtx envelopes[2];
    for(auto& matrix:envelopes) for(auto& row:matrix) for(float& v:row) v=NAN;
    tree.mEnvelopeCnt=2; tree.mEnvelopeMixCnt=counts; tree.mEnvelopeMixIdx=indices;
    tree.mEnvelopeMixWeight=weights; tree.mInvJointMtx=inverse;
    buffer.mJointTree=&tree; buffer.mWorldMatrices=joints; buffer.mScaleFlags=jointScale;
    buffer.mWeightEnvelopeMatrices=envelopes; buffer.mEnvelopeScaleFlags=envelopeScale;
    buffer.calcWeightEnvelopeMtx();
    require(near(envelopes[0][0][0],1.75f) && near(envelopes[0][0][3],17) &&
            near(envelopes[0][1][3],25.25f) && near(envelopes[0][2][3],33.5f),"weighted inverse-bind skinning");
    require(near(envelopes[1][0][0],1) && near(envelopes[1][0][3],8) &&
            near(envelopes[1][0][1],0) && envelopeScale[0]==0 && envelopeScale[1]==1,"envelope reset and scale flags");
    Mtx shear={{2,1,0,10},{0,3,1,20},{0,0,4,30}};
    Mtx33 normal={}; J3DPSCalcInverseTranspose(shear,normal);
    for(int r=0;r<3;++r) for(int c=0;c<3;++c) {
        float v=0; for(int k=0;k<3;++k) v+=shear[k][r]*normal[k][c];
        require(near(v,r==c ? 1:0),"normal matrix inverse transpose");
    }
    Mtx33 copy={}; J3DPSMtx33Copy(normal,copy); require(near(copy[2][1],normal[2][1]),"normal matrix copy");
    J3DPSMtx33CopyFrom34(shear,copy); require(copy[1][2]==1 && copy[2][2]==4,"3x4 normal extraction");
    Vec v={1,2,3}; require(J3DCalcZValue(&base,v)==33,"depth sort transform");
    Vec scale={2,3,4}; J3DScaleNrmMtx(shear,scale); J3DScaleNrmMtx33(copy,scale);
    require(shear[0][0]==4 && shear[0][1]==3 && shear[2][2]==16 && shear[0][3]==10 && copy[2][2]==16,"normal column scaling");
    Mtx44 proj={{2,0,0,0},{0,3,0,0},{0,0,4,1},{0,0,-1,0}}; Mtx result;
    J3DMtxProjConcat(base,proj,result);
    require(result[0][0]==2 && result[0][2]==-10 && result[2][2]==-26 && result[2][3]==1,"3x4 by 4x4 projection product");
    Mtx billboard={{2,1,3,10},{1,3,2,20},{4,2,4,30}};
    J3DCalcBBoardMtx(billboard);
    require(billboard[0][1]==0 && billboard[0][2]==0 && billboard[2][0]==0 && billboard[2][1]==0 && billboard[0][3]==10,"billboard clears off-diagonal terms only");
    Quaternion q={0,0,0,1}, opposite={0,0,0,-1}, lerped={};
    JMAQuatLerp(&q,&opposite,0.5f,&lerped);
    require(lerped.w==1 && lerped.x==0,"quaternion interpolation chooses shortest sign");
    std::puts("Native J3D world, camera, normal, and projection matrices passed.");
}
