#include "Dolphin/mtx.h"
#include <cmath>

// Portable equivalents of the SDK paired-single entry points. Keep the input
// vector in temporaries because the game also uses in-place transforms.
extern "C" void PSMTXMultVecSR(const Mtx matrix,const Vec* input,Vec* output) {
    const float x=input->x,y=input->y,z=input->z;
    output->x=matrix[0][0]*x+matrix[0][1]*y+matrix[0][2]*z;
    output->y=matrix[1][0]*x+matrix[1][1]*y+matrix[1][2]*z;
    output->z=matrix[2][0]*x+matrix[2][1]*y+matrix[2][2]*z;
}
extern "C" void PSMTXRotAxisRad(Mtx matrix,const Vec* axis,float radians) {
    const float inverse=1.0f/std::sqrt(axis->x*axis->x+axis->y*axis->y+axis->z*axis->z);
    const float x=axis->x*inverse,y=axis->y*inverse,z=axis->z*inverse;
    const float s=std::sin(radians),c=std::cos(radians),t=1.0f-c;
    matrix[0][0]=t*x*x+c; matrix[0][1]=t*x*y-s*z; matrix[0][2]=t*x*z+s*y; matrix[0][3]=0;
    matrix[1][0]=t*x*y+s*z; matrix[1][1]=t*y*y+c; matrix[1][2]=t*y*z-s*x; matrix[1][3]=0;
    matrix[2][0]=t*x*z-s*y; matrix[2][1]=t*y*z+s*x; matrix[2][2]=t*z*z+c; matrix[2][3]=0;
}
