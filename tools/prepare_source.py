#!/usr/bin/env python3
"""Materialize a pinned, patched build copy; never edit the reference checkout."""
import argparse
import hashlib
import io
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
from scope_switches import scope_switches
import jaudio_patches
from sync_tree import sync_tree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    revision = (project / "upstream-revision.txt").read_text().strip()
    output = args.output.resolve()
    source = args.source.resolve()
    if output == source or source.is_relative_to(output) or output.is_relative_to(source):
        raise SystemExit("Prepared output must be separate from the reference checkout")
    patch_files = [Path(__file__), project / "tools/scope_switches.py", project / "tools/switch_scope_files.txt",
                   project / "tools/jaudio_patches.py"]
    signature = revision + ":" + hashlib.sha256(b"".join(p.read_bytes() for p in patch_files)).hexdigest()
    stamp = output / ".p2-prepared"
    if stamp.exists() and stamp.read_text() == signature:
        return
    # Generate into a scratch tree, then mirror only changed files into the
    # stable output so a patch edit recompiles only what it touched.
    destination = output.with_name(output.name + ".staging")
    shutil.rmtree(destination, ignore_errors=True)
    data = subprocess.check_output(["git", "-C", str(source), "archive", revision, "src", "include"])
    destination.mkdir(parents=True)
    # The archive comes from the explicit pinned local commit, not a download.
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(destination, filter="data")

    def sub_exact(pattern, repl, text, expected, label, **kwargs):
        """re.subn that fails closed: struct-layout rewrites must not silently miss fields."""
        text, count = re.subn(pattern, repl, text, **kwargs)
        if count != expected:
            raise RuntimeError(f"Patch drift in {label}: expected {expected} substitutions, got {count}")
        return text

    def replace(path, old, new, count=1):
        file = destination / path
        text = file.read_text()
        if text.count(old) != count:
            raise RuntimeError(f"Patch drift in {path}: expected {count} occurrences of {old!r}")
        file.write_text(text.replace(old, new))

    replace("include/types.h", "typedef signed long s32;", "typedef int32_t s32;")
    replace("include/types.h", "typedef unsigned long u32;", "typedef uint32_t u32;")
    replace("include/types.h", "typedef u32 size_t;", "// size_t comes from the host standard library.")
    replace("include/types.h", "#define NULL    ((void*)0)", "// NULL comes from the host standard library.")
    replace("include/types.h", "#define nullptr 0", "// Use the host C++ nullptr keyword.")
    replace("include/types.h", "((X) & ~((N) - 1))", "((uintptr_t)(X) & ~((uintptr_t)(N) - 1))")
    replace("include/types.h", "ALIGN_PREV(((X) + (N) - 1), N)", "ALIGN_PREV(((uintptr_t)(X) + (uintptr_t)(N) - 1), N)")
    # Host libc/libc++ replace CodeWarrior MSL. Do not mix their FILE, varargs,
    # allocation or iterator declarations in one translation unit.
    for header in ("stdio", "stdlib", "ctype", "errno", "float", "limits", "locale", "signal", "stdarg", "wchar", "math"):
        (destination / f"include/stl/{header}.h").write_text(f"#pragma once\n#include <{header}.h>\n")
    for header in ("utility", "iterator", "functional", "algorithm"):
        (destination / f"include/stl/{header}.h").write_text(f"#pragma once\n#include <{header}>\n")
    (destination / "include/PowerPC_EABI_Support/MSL_C++/MSL_Common/Include/algorithm.h").write_text("#pragma once\n#include <algorithm>\n")
    (destination / "include/PowerPC_EABI_Support/MSL_C++/MSL_Common/Include/msl_memory.h").write_text("#pragma once\n#include <memory>\n")
    (destination / "include/Dolphin/stl.h").write_text("#pragma once\n#include <stdio.h>\n#include <string.h>\n#include <stdarg.h>\n")
    (destination / "include/fdlibm.h").write_text("#pragma once\n#include <math.h>\n")
    (destination / "include/mem.h").write_text("#pragma once\n#include <string.h>\n")
    (destination / "include/stl/mem.h").write_text("#pragma once\n#include <string.h>\n")
    (destination / "include/stl/string.h").write_text('''#pragma once
#include <string.h>
#include <strings.h>
#define stricmp strcasecmp
#define IS_SAME_STRING(a,b) (strcmp((a),(b)) == 0)
#define IS_SAME_STRING_N(a,b,n) (strncmp((a),(b),(n)) == 0)
#define IS_SAME_STRING_PREFIX(a,b) IS_SAME_STRING_N(a,b,sizeof(b)-1)
''')
    replace("include/JSystem/JSupport/JSUStream.h", "enum JSUStreamSeekFrom { SEEK_SET = 0, SEEK_CUR, SEEK_END };",
            "typedef int JSUStreamSeekFrom; // Host SEEK_* constants have the same values.")
    replace("include/JSystem/JGeometry.h", "vec.set<T>", "vec.template set<T>", count=2)
    replace("include/stream.h", '#include "types.h"', '#include "types.h"\n#include "JSystem/JKernel/JKRDvdRipper.h"')
    # Binary streams swap when the data's byte order differs from the host's;
    # the console compared against its own big-endian order.
    replace("include/stream.h", "bool differentEndian() { return mEndian != STREAM_BIG_ENDIAN; }",
            "bool differentEndian() { return (mEndian == STREAM_BIG_ENDIAN) != (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__); }")
    replace("include/Matrixf.h", "inline void Vector3f::operator*=", "template <> inline void Vector3f::operator*=")
    replace("include/Container.h", "struct ArrayContainer : public Container<T> {",
            "struct ArrayContainer : public Container<T> {\n\tusing Container<T>::_18;\n\tusing Container<T>::mName;")
    # These void* values are iterator indices, never addresses to dereference.
    replace("include/Container.h", "(s32)index", "(intptr_t)index", count=2)
    replace("include/MonoObjectMgr.h", "(int)idx", "(intptr_t)idx", count=3)
    replace("include/MonoObjectMgr.h", "_18          = 1;", "this->_18    = 1;")
    replace("include/PSAutoBgm/PSAutoBgm.h", ": JSULink(&_10)", ": JSULink<T>(&_10)")
    replace("include/PSSystem/MutexList.h", "bool result = append(link);", "bool result = this->append(link);")
    replace("include/PSSystem/MutexList.h", "bool result = remove(link);", "bool result = this->remove(link);")
    replace("include/Quat.h", "Quat(Quat& other);", "Quat(const Quat& other);")
    replace("src/sysCommonU/sysMath.cpp", "Quat::Quat(Quat& quat)", "Quat::Quat(const Quat& quat)")
    replace("include/Dolphin/__start.h", "u16 Pad3Button : PAD3_BUTTON_ADDR;", "extern u16 Pad3Button;")
    replace("src/JSystem/J2D/J2DPrint.cpp", '#include "PowerPC_EABI_Support\\MSL_C\\MSL_Common\\strtold.h"', '#include <stdlib.h>')
    replace("src/JSystem/J2D/J2DPrint.cpp", '#include "PowerPC_EABI_Support\\MSL_C\\MSL_Common\\strtoul.h"', '')
    replace("include/InfoList.h", "List* list = new CarryInfoList;", "List* list = new List;")
    replace("include/Game/BasePelletMgr.h", "pellet->mSlotIndex", "static_cast<T*>(pellet)->mSlotIndex", count=4)
    replace("include/Game/BasePelletMgr.h", "Pellet* pellet = mMonoObjectMgr.getAt(i);", "T* pellet = mMonoObjectMgr.getAt(i);")
    replace("include/Game/EnemyMgrBase.h", "getEnemy((int)index)", "getEnemy((intptr_t)index)")
    replace("include/Game/gameGeneratorCache.h", "(u32)buffer", "(uintptr_t)buffer", count=2)
    replace("include/Radar.h", "mCaveID  = nullptr;", "mCaveID  = 0;")
    replace("include/Game/EnemyIterator.h", "mCondition->satisfy(mContainer->get(mIndex))",
            "mCondition->satisfy(static_cast<T*>(mContainer->getObject(mIndex)))")
    (destination / "include/PowerPC_EABI_Support/MSL_C/MSL_Common/arith.h").write_text("#pragma once\n#include <stdlib.h>\ninline int _abs(int x) { return abs(x); }\n")
    replace("include/JSystem/JStudio/functionvalue.h", "mWordData((u32)data)", "mRawData(data)")
    replace("include/JSystem/JGadget/vector.h", "((int)mEnd - (int)mBegin) / 4", "mEnd - mBegin")
    for entity in ("TPikmin", "TChappy"):
        replace(f"include/ebi/title/Entities/{entity}.h", "\tE3DAnimRes* getAnimRes(int);", "")
    replace("include/JSystem/JGadget/enumerator.h", "TEnumerator2<TLinkList<T, I>::iterator", "TEnumerator2<typename TLinkList<T, I>::iterator", count=2)
    replace("include/JSystem/JGadget/enumerator.h", "TEnumerator2<TLinkList<T, I>::const_iterator", "TEnumerator2<typename TLinkList<T, I>::const_iterator", count=2)
    replace("include/JSystem/JGadget/enumerator.h", "TEnumerator<T::iterator>", "TEnumerator<typename T::iterator>")
    replace("include/Game/P2JST/ObjectSystem.h", "virtual char* JSGGetName()", "virtual const char* JSGGetName()")
    replace("src/sysGCU/JSTObjectSystem.cpp", "char* ObjectSystem::JSGGetName()", "const char* ObjectSystem::JSGGetName()")
    replace("include/JSystem/J3D/J3DPE.h", "u8 getZCompLoc() const { return nullptr; }", "u8 getZCompLoc() const { return 0; }")
    replace("include/JSystem/JKernel/JKRAram.h", "virtual size_t getExpandedResSize", "virtual u32 getExpandedResSize")
    replace("src/JSystem/JKernel/JKRAramArchive.cpp", "size_t JKRAramArchive::getExpandedResSize", "u32 JKRAramArchive::getExpandedResSize")
    replace("include/JSystem/JStudio/functionvalue.h", '#include "fdlibm.h"', '#include <math.h>')
    replace("include/JSystem/JStudio/functionvalue.h", "typedef int ptrdiff_t;", "// ptrdiff_t is supplied by the host.")
    replace("include/JSystem/JStudio/stb.h", "int seqAddr = (s32)getSequence();", "intptr_t seqAddr = (intptr_t)getSequence();")
    # Skipping a cutscene runs a destroyed stb object (iPad: pure virtual from
    # process_sequence_). MWCC resets the vtable in every destructor, so the
    # object ends as a plain stb::TObject whose virtuals are no-ops; clang drops
    # the store in this empty destructor and leaves JStudio::TObject's table
    # (pure do_paragraph, others using the freed adaptor). An opaque call keeps
    # the store (src/host_scratch.cpp).
    replace("src/JSystem/JStudio/stb.cpp", "TObject::~TObject()\n{\n}",
            'extern "C" void p2_keep_destroyed_vtable(void*);\nTObject::~TObject()\n{\n\tp2_keep_destroyed_vtable(this);\n}')
    replace("include/JSystem/JAudio/JAI/JAInter.h", "(u32)vec1 == 0xFFFFFFFF", "(uintptr_t)vec1 == 0xFFFFFFFF")
    replace("include/JSystem/J3D/J3DMaterial.h", "if ((u32)mMaterialAnm < 0xC0000000)",
            "if ((uintptr_t)mMaterialAnm < 0xC0000000ULL || (uintptr_t)mMaterialAnm > 0xFFFFFFFFULL)")
    replace("include/JSystem/J2D/J2DPicture.h", "(u32)animation->mVtxColorIndexData[b]->mData", "(uintptr_t)animation->mVtxColorIndexData[b]->mData")
    # This decomp cast assigns a temporary base object on Clang. Update the
    # actual material color, including alpha, and unpack numeric RGBA16 values.
    replace("include/JSystem/J2D/J2DGXColorS10.h",
            "(GXColorS10)* this = (GXColorS10)other;",
            "static_cast<GXColorS10&>(*this) = static_cast<const GXColorS10&>(other);")
    file = destination / "include/JSystem/J2D/J2DGXColorS10.h"
    text, count = re.subn(r"J2DGXColorS10\(const u64& other\)\s*\{.*?\n\t\}",
        "J2DGXColorS10(const u64& other)\n\t{\n\t\tr=s16(other >> 48); g=s16(other >> 32); b=s16(other >> 16); a=s16(other);\n\t}",
        file.read_text(), flags=re.S)
    if count != 1: raise RuntimeError("J2D packed color patch drift")
    file.write_text(text)
    # Texture copies may be farther than 4 GB apart on the host. Preserve disk
    # header sizes and keep the copied image/palette addresses in a side table.
    replace("include/JSystem/J3D/J3DTypes.h", '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"')
    for command in ("mBPCommand1", "mBPCommand2"):
        replace("include/JSystem/J3D/J3DTypes.h", f"*(u32*)&{command}", f"p2_read_big<u32>(&{command})")
    for constructor in ("J3DTevStage()", "J3DTevStage(const J3DTevStageInfo& info)"):
        replace("include/JSystem/J3D/J3DTypes.h", constructor + "\n\t{",
                constructor + "\n\t{\n\t\tmemset(this, 0, sizeof(*this)); // setters update individual packed bits")
    replace("include/JSystem/J3D/J3DColorBlock.h", "*(u32*)color", "p2_read_big<u32>(color)", count=2)
    replace("include/JSystem/J3D/J3DColorBlock.h", "*(u32*)(color + 1)", "p2_read_big<u32>(color + 1)", count=2)
    replace("src/JSystem/J3D/J3DTevs.cpp", "*(u32*)(static_cast<u8*>(p1) + 1)", "p2_read_big<u32>(static_cast<u8*>(p1) + 1)")
    replace("src/Dolphin/gd/GDBase.c", "((u32)__GDCurrentDL->data & 31)", "((uintptr_t)__GDCurrentDL->data & 31)")
    replace("include/Dolphin/gd.h", "\t__GDWrite(((u8*)&data)[0]);\n\t__GDWrite(((u8*)&data)[1]);\n\t__GDWrite(((u8*)&data)[2]);\n\t__GDWrite(((u8*)&data)[3]);",
            "\tu32 bits; memcpy(&bits, &data, sizeof(bits));\n\t__GDWriteU32(bits);")
    replace("include/JSystem/ResTIMG.h", '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"')
    for scalar, member in (("u16", "mSizeX"), ("u16", "mSizeY"), ("u16", "mPaletteEntryCount"),
                           ("u32", "mPaletteOffset"), ("s16", "mLODBias"), ("u32", "mImageDataOffset")):
        replace("include/JSystem/ResTIMG.h", f"{scalar} {member};", f"P2Big<{scalar}> {member};")
    replace("include/JSystem/J3D/J3DTexture.h", "mRes = res;", "mRes = res;\n\t\tfor (u16 i = 0; i < mNum; ++i) p2_texture_forget(&mRes[i]);")
    replace("include/JSystem/J3D/J3DTexture.h", "virtual ~J3DTexture() { }", "virtual ~J3DTexture() { for (u16 i = 0; i < mNum; ++i) p2_texture_forget(&mRes[i]); }")
    replace("include/JSystem/J3D/J3DTexture.h",
            "mRes[index].mImageDataOffset = (int)(((u32)image + mRes[index].mImageDataOffset) - (u32)&mRes[index]);\n\t\tmRes[index].mPaletteOffset   = (int)(((u32)image + mRes[index].mPaletteOffset) - (u32)&mRes[index]);",
            "p2_texture_copy(&mRes[index], image, image->mImageDataOffset, image->mPaletteOffset);")
    replace("src/JSystem/JUtility/JUTTexture.cpp", "(void*)((u32)mTexInfo + mTexInfo->mImageDataOffset)",
            "p2_texture_image(mTexInfo, mTexInfo->mImageDataOffset)", count=2)
    replace("src/JSystem/JUtility/JUTTexture.cpp", "(void*)((u32)mTexInfo + mTexInfo->mPaletteOffset)",
            "p2_texture_palette(mTexInfo, mTexInfo->mPaletteOffset)", count=2)
    replace("src/JSystem/JUtility/JUTTexture.cpp", "image += ((mTexInfo->mImageDataOffset) ? mTexInfo->mImageDataOffset : 0x20);",
            "image = (u8*)p2_texture_image(mTexInfo, mTexInfo->mImageDataOffset ? u32(mTexInfo->mImageDataOffset) : 0x20);", count=2)
    replace("src/JSystem/J3D/J3DTevs.cpp", "(u8*)resTIMG + resTIMG->mImageDataOffset", "p2_texture_image(resTIMG, resTIMG->mImageDataOffset)")
    replace("src/JSystem/J3D/J3DTevs.cpp", "(u8*)resTIMG + resTIMG->mPaletteOffset", "p2_texture_palette(resTIMG, resTIMG->mPaletteOffset)")
    replace("src/JSystem/J3D/J3DDrawBuffer.cpp", "((u32)texture->getResTIMG(texNo) + texture->getResTIMG(texNo)->mImageDataOffset) >> 5",
            "(uintptr_t)p2_texture_image(texture->getResTIMG(texNo), texture->getResTIMG(texNo)->mImageDataOffset) >> 5")
    replace("src/JSystem/J2D/J2DAnimation.cpp", "((u8*)imageResource) + imageResource->mPaletteOffset",
            "p2_texture_palette(imageResource, imageResource->mPaletteOffset)")
    replace("include/JSystem/JKernel/JKRHeap.h", "(CMemBlock*)((u32)data + -0x10)", "static_cast<CMemBlock*>(data) - 1")
    replace("include/JSystem/JKernel/JKRHeap.h", "inline void* operator new(size_t size, void* mem)\n{\n\treturn mem;\n}",
            "// Placement new is supplied by the host <new> header.")
    replace("include/JSystem/JUtility/JUTGamePad.h", "(*Callback)(EPadPort, u32)", "(*Callback)(EPadPort, uintptr_t)")
    replace("include/JSystem/JUtility/JUTGamePad.h", "static u32 sCallbackArg;", "static uintptr_t sCallbackArg;")
    replace("include/JSystem/JUtility/JUTGamePad.h", "sCallbackArg = (u32)param_0;", "sCallbackArg = (uintptr_t)param_0;")
    replace("src/JSystem/JUtility/JUTGamePad.cpp", "u32 JUTGamePad::C3ButtonReset::sCallbackArg;", "uintptr_t JUTGamePad::C3ButtonReset::sCallbackArg;")
    replace("include/JSystem/J2D/J2DAnm.h", "_60                = nullptr;", "_60                = 0;")
    replace("include/JSystem/JGadget/search.h", '#include "types.h"', '#include "types.h"\n#include <algorithm>')
    (destination / "include/sqrt.h").write_text('''#pragma once
#include "types.h"
#include <math.h>
#define FRSQRTE(input, output) (*(output) = (f32)__frsqrte(input))
inline f32 sqrtfInPlace(f32& value) {
    if (value > 0.0f) value = ::sqrtf(value);
    return value;
}
inline f32 sqrtfClamped(f32 value) { return value > 0.0f ? ::sqrtf(value) : 0.0f; }
''')
    replace("include/JSystem/JSupport/JSU.h", "(T*)((s32)base + offset)", "(T*)((uintptr_t)base + offset)")
    replace("include/JSystem/JSupport/JSU.h", "(T*)((s32)(base) + (s32)(offset))", "(T*)((uintptr_t)base + (uintptr_t)offset)")
    # MWCC accepted unqualified members from dependent template bases.
    replace("include/JSystem/JGeometry.h", "struct TBox2 : TBox<TVec2<T> > {",
            "struct TBox2 : TBox<TVec2<T> > {\n\tusing TBox<TVec2<T> >::i;\n\tusing TBox<TVec2<T> >::f;")
    replace("include/JSystem/JSupport/JSUList.h", "struct JSUTree : public JSUList<T>, public JSULink<T> {",
            "struct JSUTree : public JSUList<T>, public JSULink<T> {\n"
            "\tusing JSUList<T>::getFirstLink;\n\tusing JSUList<T>::getLast;\n\tusing JSUList<T>::mLinkCount;\n"
            "\tusing JSULink<T>::mNext;\n\tusing JSULink<T>::mValue;\n\tusing JSULink<T>::getPrev;\n\tusing JSULink<T>::getList;")
    replace("include/Game/StateMachine.h", "T::StateType* state = static_cast<T::StateType*>",
            "typename T::StateType* state = static_cast<typename T::StateType*>")
    replace("include/JSystem/JGadget/linklist.h", "while (!empty())", "while (!this->empty())")
    replace("include/JSystem/JGadget/linklist.h", "T* item = &front();", "T* item = &this->front();")
    replace("include/JSystem/JGadget/linklist.h", "TLinkList<T, Offset>::iterator Erase_destroy", "typename TLinkList<T, Offset>::iterator Erase_destroy")
    replace("include/JSystem/JGadget/linklist.h", "TLinkList<T, Offset>::iterator spC(Erase(param_0));", "typename TLinkList<T, Offset>::iterator spC(this->Erase(param_0));")
    file = destination / "include/JSystem/JMath.h"
    text, count = re.subn(r"(?m)^f32 (TSinCosTable<\d+, f32>::(?:sin|cos)Short)", r"template <> inline f32 \1", file.read_text())
    if count != 6:
        raise RuntimeError("JMath specialization patch drift")
    file.write_text(text)
    # P2's LENGTH is the entry count (2048), not log2(entry count).
    replace("include/JSystem/JMath.h", "((1 << LENGTH) - 1)", "(LENGTH - 1)", count=2)
    replace("include/JSystem/JMath.h",
            "return *(f32*)(void*)((value >> 9) | 0x3F800000) - 1.0f;",
            "u32 bits = (value >> 9) | 0x3F800000; f32 result; memcpy(&result, &bits, sizeof(result)); return result - 1.0f;")
    for name in ("atan_", "atan2_"):
        replace("src/JSystem/JMath/JMATrigonometric.cpp", f"f32 TAtanTable<1024, f32>::{name}", f"template <> f32 TAtanTable<1024, f32>::{name}")
    replace("src/sysCommonU/sysMath.cpp", "register f32 reg_f0;", "register f32 reg_f0 = (f32)__frsqrte(x);")

    # These functions otherwise become no-ops or return uninitialized registers
    # under Clang because their only implementation is MWCC paired-single asm.
    (destination / "include/JSystem/JMath/Inline.h").write_text('''#pragma once
#include "Dolphin/mtx.h"
namespace JMathInlineVEC {
inline void PSVECAdd(const Vec* a, const Vec* b, Vec* out) {
    const Vec v = {a->x+b->x, a->y+b->y, a->z+b->z}; *out = v;
}
inline void PSVECSubtract(const Vec* a, const Vec* b, Vec* out) {
    const Vec v = {a->x-b->x, a->y-b->y, a->z-b->z}; *out = v;
}
inline void PSVECScale(const Vec* a, Vec* out, f32 scale) {
    const Vec v = {a->x*scale, a->y*scale, a->z*scale}; *out = v;
}
inline f32 PSVECDotProduct(const Vec* a, const Vec* b) {
    return a->x*b->x + a->y*b->y + a->z*b->z;
}
inline f32 PSVECSquareMag(const Vec* a) { return PSVECDotProduct(a, a); }
}
''')
    # These assembly-only routines otherwise silently leave world/normal matrices
    # at zero on ARM64, including the black-plane joint that drives title camera.
    def native_body(path, name, body):
        file = destination / path
        text, count = re.subn(r"((?:ASM )?void " + name + r"\([^\n]*\)\s*)\{.*?^\}",
                              lambda m: m[1] + "{\n" + body + "\n}", file.read_text(), flags=re.M | re.S)
        if count != 1: raise RuntimeError("Native matrix patch drift: " + name)
        file.write_text(text)
    native_body("src/JSystem/JMath/JMath.cpp", "JMAMTXApplyScale",
                "\tfor (int r=0;r<3;++r) {\n\t\tdst[r][0]=src[r][0]*xScale; dst[r][1]=src[r][1]*yScale;\n"
                "\t\tdst[r][2]=src[r][2]*zScale; dst[r][3]=src[r][3];\n\t}")
    replace("src/JSystem/JMath/JMath.cpp", "register f32 dp;", "register f32 dp = p->x*q->x + p->y*q->y + p->z*q->z + p->w*q->w;")
    replace("include/JSystem/J3D/J3DMtxCalc.h", "J3DTransformInfo* pInfo;\n\t\tif", "J3DTransformInfo v1;\n\t\tJ3DTransformInfo* pInfo;\n\t\tif")
    replace("include/JSystem/J3D/J3DMtxCalc.h", "\t\t\tJ3DTransformInfo v1;\n", "")
    replace("include/JSystem/J3D/J3DDrawBuffer.h", "return out;", "return (*m)[2][0]*v.x + (*m)[2][1]*v.y + (*m)[2][2]*v.z + (*m)[2][3];")
    for name in ("J3DPSMtx33Copy", "J3DPSMtx33CopyFrom34"):
        file = destination / "include/JSystem/J3D/J3DTransform.h"
        text, count = re.subn(r"(inline void " + name + r"\([^\n]+\)\n)\{.*?^\}",
                              lambda m: m[1]+"{\n\tfor(int r=0;r<3;++r) for(int c=0;c<3;++c) dst[r][c]=src[r][c];\n}",
                              file.read_text(), flags=re.M|re.S)
        if count != 1: raise RuntimeError("Matrix copy patch drift")
        file.write_text(text)
    native_body("src/JSystem/J3D/J3DMtxBuffer.cpp", "J3DMtxBuffer::calcWeightEnvelopeMtx", """
    u32 cursor = 0;
    for (u32 envelope = 0; envelope < mJointTree->getWEvlpMtxNum(); ++envelope) {
        Mtx& result = mWeightEnvelopeMatrices[envelope];
        for (int r=0; r<3; ++r) for (int c=0; c<4; ++c) result[r][c]=0.0f;
        mEnvelopeScaleFlags[envelope]=1;
        for (u32 mix=0; mix<mJointTree->getWEvlpMixMtxNum(envelope); ++mix, ++cursor) {
            const u16 joint=mJointTree->getWEvlpMixIndex()[cursor];
            const f32 weight=mJointTree->getWEvlpMixWeight()[cursor];
            Mtx product;
            PSMTXConcat(mWorldMatrices[joint],mJointTree->getInvJointMtx(joint),product);
            for (int r=0; r<3; ++r) for (int c=0; c<4; ++c) result[r][c]+=weight*product[r][c];
            mEnvelopeScaleFlags[envelope] &= mScaleFlags[joint];
        }
    }
""")
    transform = "src/JSystem/J3D/J3DTransform.cpp"
    replace(transform, "\tmtx[0][0] = x;\n\tmtx[1][0] = zero;", "\tmtx[0][1]=mtx[0][2]=mtx[2][0]=mtx[2][1]=0.0f;\n\tmtx[0][0] = x;\n\tmtx[1][0] = zero;")
    native_body(transform, "J3DPSCalcInverseTranspose", """
    Mtx33 cofactor;
    for(int r=0;r<3;++r) for(int c=0;c<3;++c)
        cofactor[r][c]=src[(r+1)%3][(c+1)%3]*src[(r+2)%3][(c+2)%3]-src[(r+1)%3][(c+2)%3]*src[(r+2)%3][(c+1)%3];
    const float det=src[0][0]*cofactor[0][0]+src[0][1]*cofactor[0][1]+src[0][2]*cofactor[0][2];
    if(det==0.0f) return;
    for(int r=0;r<3;++r) for(int c=0;c<3;++c) dst[r][c]=cofactor[r][c]/det;
""")
    for name, scale in (("J3DScaleNrmMtx", "scl"), ("J3DScaleNrmMtx33", "scale")):
        native_body(transform, name, "\tfor(int r=0;r<3;++r) { mtx[r][0]*="+scale+".x; mtx[r][1]*="+scale+".y; mtx[r][2]*="+scale+".z; }")
    native_body(transform, "J3DMtxProjConcat", """
    Mtx result;
    for(int r=0;r<3;++r) for(int c=0;c<4;++c) {
        result[r][c]=0;
        for(int k=0;k<4;++k) result[r][c]+=mtx1[r][k]*mtx2[k][c];
    }
    PSMTXCopy(result,dst);
""")
    # Reviewed host address arithmetic and local C++ compatibility fixes.
    replace('src/JSystem/J2D/J2DAnmLoader.cpp', '(s32)dataPtr', '(uintptr_t)dataPtr', count=4)
    replace('src/JSystem/J2D/J2DPictureEx.cpp', '(u32)data->mData', '(uintptr_t)data->mData', count=1)
    replace('src/JSystem/J2D/J2DPictureEx.cpp', '(int)data->mData', '(intptr_t)data->mData', count=1)
    replace('src/JSystem/J2D/J2DWindowEx.cpp', '(u32)puVar1->mData', '(uintptr_t)puVar1->mData', count=2)
    replace('src/JSystem/J3D/J3DDrawBuffer.cpp', '(u32)pMaterialAnm', '(uintptr_t)pMaterialAnm', count=1)
    replace('src/JSystem/J3D/J3DAnmLoader.cpp', '(u32)animation->mAnmVtxColorIndexData', '(uintptr_t)animation->mAnmVtxColorIndexData', count=4)
    replace('src/JSystem/J3D/J3DAnmLoader.cpp', 'mNameTab2Offset != nullptr', 'mNameTab2Offset != 0', count=1)
    replace('src/JSystem/J3D/J3DJoint.cpp', 'mCallBackUserData = nullptr', 'mCallBackUserData = 0', count=1)
    replace('src/JSystem/J3D/J3DJoint.cpp', '_08               = nullptr', '_08               = 0', count=1)
    replace('src/JSystem/J3D/J3DTevs.cpp', '(u32)ptr', '(uintptr_t)ptr', count=2)
    replace('src/plugProjectKandoU/onyonMgr.cpp', '\n\t\tonyon = new Onyon;', '\n\t\tOnyon* onyon = new Onyon;', count=2)
    replace('src/plugProjectKandoU/pelletState.cpp', '\n\t\tcheck = execMove', '\n\t\tint check = execMove', count=2)
    replace('src/plugProjectYamashitaU/enemyStoneDrawInfo.cpp', '\n\t\tintRatio = (int)(ratio * 100.0f);', '\n\t\tint intRatio = (int)(ratio * 100.0f);', count=1)
    replace('src/plugProjectKandoU/radarInfo.cpp', 'mCaveID  = nullptr', 'mCaveID  = 0', count=1)
    replace('src/plugProjectKandoU/updateMgr.cpp', 'mClientListA[i] = nullptr', 'mClientListA[i] = 0', count=1)
    replace('src/plugProjectKandoU/updateMgr.cpp', 'mClientListB[i] = nullptr', 'mClientListB[i] = 0', count=1)
    replace('src/plugProjectKandoU/vsCardMgr.cpp', 'GXSetCurrentMtx(nullptr)', 'GXSetCurrentMtx(0)', count=1)
    replace('src/plugProjectOgawaU/ogObjAnaDemo.cpp', 'mUnusedObj         = nullptr', 'mUnusedObj         = 0', count=1)
    replace('src/sysGCU/dvdThread.cpp', "(u32)msg == 'DTLF'", "(uintptr_t)msg == 'DTLF'", count=1)
    replace('src/sysGCU/pikmin2MemoryCardMgr.cpp', '(u32)mIconImageFile', '(uintptr_t)mIconImageFile', count=1)
    replace('src/plugProjectYamashitaU/vtxAnm.cpp', '(u32)ptr - (u32)dispList', '(uintptr_t)ptr - (uintptr_t)dispList', count=1)
    replace('src/plugProjectYamashitaU/kochappyState.cpp', "(int)stateArg == 'rand'", "(intptr_t)stateArg == 'rand'", count=1)
    replace('src/sysGCU/bootSection.cpp', 'runWait(&waitLoadResource)', 'runWait(&BootSection::waitLoadResource)', count=2)
    replace('src/sysGCU/pikmin2THPPlayer.cpp', '_C8(this, &loadResource)', '_C8(this, &THPPlayer::loadResource)', count=1)
    replace('include/Game/itemMgr.h', '(this, createModelCallback)', '(this, &FixedSizeItemMgr<T>::createModelCallback)', count=1)
    replace('include/Game/itemMgr.h', '\n\tmModelMgr->createModel(item->_188, item->_184);', '\n\treturn mModelMgr->createModel(item->_188, item->_184);', count=1)
    replace('src/JSystem/JKernel/JKRAramHeap.cpp', 'allocFromHead(size_t size)', 'allocFromHead(u32 size)', count=1)
    replace('src/JSystem/JKernel/JKRAramHeap.cpp', 'allocFromTail(size_t size)', 'allocFromTail(u32 size)', count=1)
    replace('src/JSystem/J2D/J2DPrint.cpp', '(u32)inputString', '(uintptr_t)inputString', count=2)
    replace('src/JSystem/J2D/J2DPrint.cpp', '(u32)originalInput', '(uintptr_t)originalInput', count=2)
    replace('src/JSystem/J2D/J2DPrint.cpp', '(u32)endStr', '(uintptr_t)endStr', count=4)
    replace('src/JSystem/J2D/J2DPrint.cpp', '(u32)*strPtr', '(uintptr_t)*strPtr', count=4)
    replace('src/JSystem/J3D/J3DMaterialFactory.cpp', '(u32)block.mIndTextureInfoOffset', '(uintptr_t)block.mIndTextureInfoOffset', count=1)
    replace('src/JSystem/J3D/J3DMaterialFactory.cpp', '(u32)block.mStringTableOffset', '(uintptr_t)block.mStringTableOffset', count=1)
    replace('src/JSystem/J3D/J3DMaterialFactory.cpp', '(u32)&mDisplayLists[index]', '(uintptr_t)&mDisplayLists[index]', count=1)

    replace('src/plugProjectEbisawaU/ebiOmakeMgr.cpp', '\n\t\t\t\tduration  = 5.0f', '\n\t\t\t\tu32 duration  = 5.0f', count=2)
    replace('src/plugProjectEbisawaU/ebiSaveMgr.cpp', '\n\t\tcheck = false;', '\n\t\tbool check = false;', count=1)
    replace('src/plugProjectEbisawaU/ebiScreenOmake.cpp', '\n\t\tcalc  = mAnims1', '\n\t\tf32 calc  = mAnims1', count=1)
    replace('src/plugProjectEbisawaU/ebiScreenOmake.cpp', '\n\t\talpha = calc', '\n\t\tf32 alpha = calc', count=1)
    replace('src/plugProjectKandoU/collinfo.cpp', '\n\t\tcrossProd = axisCross;', '\n\t\tVector3f crossProd = axisCross;', count=1)
    replace('src/plugProjectKandoU/aiAction.cpp', 'mActionId = ACT_NULL;\n\t\treturn;', 'mActionId = ACT_NULL;\n\t\treturn false;', count=1)
    replace('src/plugProjectKandoU/aiAction.cpp', 'if (!nextAction->applicable())', 'if (!nextAction || !nextAction->applicable())', count=1)
    replace('src/plugProjectKandoU/baseGameSection.cpp', 'u32 BaseGameSection::waitSyncLoad', 'void BaseGameSection::waitSyncLoad', count=1)
    replace('include/Game/BaseGameSection.h', 'u32 waitSyncLoad(bool);', 'void waitSyncLoad(bool);', count=1)
    replace('src/plugProjectKandoU/baseGameSection.cpp', 'pelletMgr->createManagers(nullptr)', 'pelletMgr->createManagers(0)', count=1)
    replace('src/plugProjectKandoU/interactPiki.cpp', 'piki->stimulate(swallowFlick);\n\t\treturn;', 'return piki->stimulate(swallowFlick);', count=1)
    replace('src/sysGCU/JSTObjectActor.cpp', 'return; // doesnt specify true or false', 'return false; // Movie has finished; no resource change occurred.', count=2)
    replace('src/sysGCU/JSTObjectActor.cpp', '"data-ID : %u (0x%08x)\\n", p1, (u32)p2', '"data-ID : %u (%p)\\n", p1, p2', count=1)
    replace('src/sysGCU/system.cpp', 'int System::assert_fragmentation', 'void System::assert_fragmentation', count=1)
    replace('include/System.h', 'static int assert_fragmentation', 'static void assert_fragmentation', count=1)
    replace('include/Vector3.h', '(T)absF(x)', '(T)fabs((double)x)', count=1)
    replace('include/Vector3.h', '(T)absF(y)', '(T)fabs((double)y)', count=1)
    replace('include/Vector3.h', '(T)absF(z)', '(T)fabs((double)z)', count=1)
    replace('include/Vector3.h', 'typedef Vector3<f32> Vector3f;', 'typedef Vector3<f32> Vector3f;\ntemplate <> void Vector3f::read(Stream&);\ntemplate <> void Vector3f::write(Stream&);', count=1)
    replace('src/sysCommonU/sysMath.cpp', 'Vector3f Vector3f::zero', 'template <> Vector3f Vector3f::zero', count=1)
    replace('src/sysCommonU/sysMath.cpp', 'void Vector3f::read', 'template <> void Vector3f::read', count=1)
    replace('src/sysCommonU/sysMath.cpp', 'void Vector3f::write', 'template <> void Vector3f::write', count=1)
    replace('src/sysCommonU/mapCode.cpp', 'output.writeByte((u8)getContents());', 'output.writeByte((u8)mContents);', count=1)
    replace('src/plugProjectKandoU/creatureStick.cpp', '(int)id', '(intptr_t)id', count=2)
    replace('src/plugProjectKandoU/creatureStick.cpp', '(int)in', '(intptr_t)in', count=1)
    replace('src/plugProjectKandoU/gameCPlate.cpp', '(int)index', '(intptr_t)index', count=2)
    replace('src/plugProjectKandoU/pelletMgr.cpp', '(int)mMgr->get', '(intptr_t)mMgr->get', count=2)
    replace('src/plugProjectKandoU/pelletMgr.cpp', '(u32)mMgr->getEnd()', '(uintptr_t)mMgr->getEnd()', count=2)
    replace('src/plugProjectKandoU/routeMgr.cpp', '(s16)index', '(s16)(intptr_t)index', count=1)
    replace('src/plugProjectKandoU/routeMgr.cpp', '(int)index', '(intptr_t)index', count=1)
    replace('src/plugProjectYamashitaU/enemyMgrBase.cpp', '(int)object', '(intptr_t)object', count=1)
    replace('src/plugProjectKandoU/cellPyramid.cpp', '(u32)currentLeg->mObject', '(uintptr_t)currentLeg->mObject', count=4)
    replace('src/plugProjectKandoU/cellPyramid.cpp', '(u32)legB->mObject', '(uintptr_t)legB->mObject', count=2)
    replace('include/Game/cellPyramid.h', 'u32 mPassID;                      // _A4', 'uintptr_t mPassID;                // Runtime collision identity or pass counter.', count=1)
    replace('src/plugProjectKandoU/navi.cpp', '(int)mCollTree->mPart', '(uintptr_t)mCollTree->mPart', count=1)
    replace('src/plugProjectKandoU/navi.cpp', 'abs(mFootmarks->mLastUpdateTime - gameSystem->mFrameTimer)', 'abs((s32)(mFootmarks->mLastUpdateTime - gameSystem->mFrameTimer))', count=1)
    replace('src/plugProjectKandoU/vsGS_Load.cpp', 'PSSE_SY_FLOOR_COMPLETE, nullptr', 'PSSE_SY_FLOOR_COMPLETE, 0', count=1)
    replace('src/plugProjectNishimuraU/BigTreasureState.cpp', 'PSSE_EN_BIGTAKARA_WAIT2, nullptr', 'PSSE_EN_BIGTAKARA_WAIT2, 0', count=1)
    replace('src/plugProjectKonoU/khDayEndResult.cpp', 'int counts[12]', 'u32 counts[12]', count=1)
    replace('src/plugProjectMorimuraU/hurryUp2D.cpp', '{ 255, 255, 255, calc }', '{ 255, 255, 255, (u8)calc }', count=1)
    replace('src/plugProjectOgawaU/ogObjKantei.cpp', 'og\\newScreen\\KanteiDemo.h', 'og/newScreen/KanteiDemo.h', count=1)
    replace('src/plugProjectYamashitaU/enemyInfo.cpp', '#include "extras.h"', '#include <strings.h>\n#define stricmp strcasecmp', count=1)
    replace('src/sysGCU/messageRendering.cpp', 'FAST_FLAG_SET(byte, *((u8*)mRubyBuffer + i++ + 1), 0, 8);', 'byte = (byte & ~0xff) | *((u8*)mRubyBuffer + i++ + 1);', count=1)
    replace('src/sysGCU/messageRendering.cpp', '!isprintable(byte)', '!isprint((unsigned char)byte)', count=1)
    replace('include/JSystem/JGadget/binary.h', 'return !operator==(a, b);', 'return a.mBegin != b.mBegin;', count=2)
    replace('include/JSystem/JGadget/binary.h', 'return *(TValueIterator<TParseValue_misaligned<T>, sizeof(T)>(*this) + n);', 'TValueIterator<TParseValue_misaligned<T>, sizeof(T)> it(*this); it += n; return *it;', count=1)
    replace('src/sysGCU/JSTObjectCamera.cpp', 'static f32 sFovBackup;', 'f32 sFovBackup;', count=1)
    replace('src/plugProjectKonoU/khPayDept.cpp', 'inline u64 J2DPane::getTagName() const\n{\n\treturn mTag;\n}', '// J2DPane::getTagName is defined in its header.', count=1)
    replace('src/plugProjectKonoU/khDayEndResult.cpp', 'inline u64 J2DPane::getTagName() const\n{\n\treturn mTag;\n}', '// J2DPane::getTagName is defined in its header.', count=1)
    replace('src/plugProjectKandoU/naviState.cpp', 'se_chats[naviID], nullptr', 'se_chats[naviID], 0', count=1)
    replace('src/plugProjectKandoU/naviState.cpp', 'se_novis[naviID], nullptr', 'se_novis[naviID], 0', count=1)
    replace('src/plugProjectKandoU/naviState.cpp', 'se_jumps[naviID], nullptr', 'se_jumps[naviID], 0', count=1)
    replace('src/plugProjectKandoU/naviState.cpp', 'se_kyoros[naviID], nullptr', 'se_kyoros[naviID], 0', count=1)
    replace('src/sysGCU/screenMgr.cpp', 'bool Mgr::setDispMember(og::Screen::DispMemberBase* disp)\n{\n\tif (mBackupScene) {\n\t\treturn mBackupScene->setDispMember(disp);\n\t}\n\n\treturn nullptr;\n}', 'bool Mgr::setDispMember(og::Screen::DispMemberBase* disp)\n{\n\tif (mBackupScene) {\n\t\treturn mBackupScene->setDispMember(disp);\n\t}\n\n\treturn false;\n}', count=1)
    replace('src/plugProjectKonoU/newGame2DMgr.cpp', 'bool Game2DMgr::setDispMember(og::Screen::DispMemberBase* disp)\n{\n\tif (mScreenMgr) {\n\t\treturn mScreenMgr->setDispMember(disp);\n\t} else {\n\t\treturn nullptr;\n\t}\n}', 'bool Game2DMgr::setDispMember(og::Screen::DispMemberBase* disp)\n{\n\tif (mScreenMgr) {\n\t\treturn mScreenMgr->setDispMember(disp);\n\t} else {\n\t\treturn false;\n\t}\n}', count=1)
    replace('include/JSystem/J3DU/J3DUMtxCache.h', '#include "JSystem/J3D/J3DJoint.h"', '#include "JSystem/J3D/J3DModel.h"\n#include "JSystem/J3D/J3DJoint.h"', count=1)
    replace('src/JSystem/JFramework/JFWDisplay.cpp', '(int)msg', '(s32)(intptr_t)msg', count=2)
    replace('src/JSystem/JMessage/processor.cpp', 'case -1:', 'case (u32)-1:', count=1)
    replace('src/JSystem/JMessage/processor.cpp', 'case -2:', 'case (u32)-2:', count=1)
    replace('src/JSystem/JMessage/processor.cpp', '(u32)data', '(uintptr_t)data', count=2)
    replace('src/JSystem/JMessage/resource.cpp', '(u32)pData', '(uintptr_t)pData', count=1)
    replace('src/JSystem/JStudio/stb-data-parse.cpp', '(int)getRaw()', '(uintptr_t)getRaw()', count=1)
    replace('src/JSystem/JStudio/stb-data-parse.cpp', '(int)next', '(uintptr_t)next', count=2)
    replace('src/JSystem/JStudio/stb.cpp', '(u32)temp', '(uintptr_t)temp', count=1)
    replace('src/JSystem/JStudio/stb.cpp', '(u32)dataID.getRaw()', '(uintptr_t)dataID.getRaw()', count=1)
    # Console layout put CreatureParms::mCreatureProps.mProps at offset 0; the host puts the vtable there.
    replace('include/Game/Entities/Kabuto.h', '((Parameters*)this)->read(stream);', 'mCreatureProps.mProps.read(stream);', count=1)
    # JStudio cutscene data (STB/FVB) is big-endian and read in place.
    replace('include/JSystem/JStudio/stb-data.h', '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"', count=1)
    replace('include/JSystem/JStudio/stb-data.h', 'u16 _08[3];         // _08\n\t\tu16 mTargetVersion;', 'P2Big<u16> _08[3];  // _08\n\t\tP2Big<u16> mTargetVersion;', count=1)
    for field in ('u16 mByteOrder;', 'u16 mVersion;', 'u32 _08;', 'u32 mBlockNum;', 'u32 mSize;', 'u32 mType;', 'u16 mFlag;', 'u16 _00;'):
        replace('include/JSystem/JStudio/stb-data.h', field, 'P2Big<%s> %s' % tuple(field.split(' ', 1)), count=1)
    replace('include/JSystem/JStudio/stb-data.h', 'u16 mIDSize;', 'P2Big<u16> mIDSize;', count=2)
    replace('include/JSystem/JStudio/fvb-data.h', '#include "JSystem/JGadget/binary.h"', '#include "JSystem/JGadget/binary.h"\n#include "p2_endian.h"', count=1)
    for field in ('u32 mSize;', 'u16 mType;', 'u16 mIDSize;', 'u16 mByteOrder;', 'u16 mVersion;', 'u32 _08;', 'u32 mBlockNumber;'):
        replace('include/JSystem/JStudio/fvb-data.h', field, 'P2Big<%s> %s' % tuple(field.split(' ', 1)), count=1)
    replace('include/JSystem/JStudio/stb-data-parse.h', 'return *(u32*)get();', 'return p2_read_big<u32>(get());', count=1)
    replace('src/JSystem/JGadget/binary.cpp', 'u32 bufferValue = *(u16*)inputBuffer;', 'u32 bufferValue = p2_read_big<u16>(inputBuffer);', count=1)
    replace('src/JSystem/JGadget/binary.cpp', '*(u16*)((u8*)inputBuffer + 2)', 'p2_read_big<u16>((u8*)inputBuffer + 2)', count=2)
    replace('src/JSystem/JGadget/binary.cpp', '*(u32*)((u8*)inputBuffer + 4)', 'p2_read_big<u32>((u8*)inputBuffer + 4)', count=1)
    replace('include/JSystem/JGadget/binary.h', '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"', count=1)
    # Every misaligned / big-endian parse reads GameCube data (cutscene user data: animation and command IDs).
    replace('include/JSystem/JGadget/binary.h', 'struct TParseValue_endian_big_ : public TParseValue_raw_<T> {\n\tstatic T parse(const void* data) { return TParseValue_raw_<T>::parse(data); }',
            'struct TParseValue_endian_big_ : public TParseValue_raw_<T> {\n\tstatic T parse(const void* data) { return p2_read_big<T>(data); }', count=1)
    replace('include/JSystem/JGadget/binary.h', 'struct TParseValue_misaligned_ : public TParseValue_raw_<T> {\n\ttypedef T ParseType;\n\tstatic T parse(const void* data) { return TParseValue_raw_<T>::parse(data); }',
            'struct TParseValue_misaligned_ : public TParseValue_raw_<T> {\n\ttypedef T ParseType;\n\tstatic T parse(const void* data) { return p2_read_big<T>(data); }', count=1)
    replace('src/JSystem/JStudio/stb.cpp', 'setFlag_operation(*(u32*)content);', 'setFlag_operation(p2_read_big<u32>(content));', count=1)
    replace('src/JSystem/JStudio/stb.cpp', 'setWait(*(u32*)content);', 'setWait(p2_read_big<u32>(content));', count=1)
    replace('src/JSystem/JStudio/stb.cpp', 'getSequenceOffset(*(s32*)content);', 'getSequenceOffset(p2_read_big<s32>(content));', count=1)
    fvb = 'src/JSystem/JStudio/fvb.cpp'
    replace(fvb, 'u32 i       = *(u32*)content;', 'u32 i       = p2_read_big<u32>(content);', count=1)
    replace(fvb, 'u32 size         = *(u32*)ptr;', 'u32 size         = p2_read_big<u32>(ptr);', count=1)
    replace(fvb, 'u32 i                                           = content[0];', 'u32 i                                           = p2_read_big<u32>(content);', count=1)
    replace(fvb, 'u32 index        = *ptr;', 'u32 index        = p2_read_big<u32>(ptr);', count=1)
    replace(fvb, 'const f32* arr = static_cast<const f32*>(pContent);', 'const P2Big<f32>* arr = static_cast<const P2Big<f32>*>(pContent);', count=1)
    for enum in ('TEProgress', 'TEAdjust', 'TEInterpolate'):
        replace(fvb, '*static_cast<const TFunctionValue::%s*>(pContent)' % enum, '(TFunctionValue::%s)p2_read_big<u32>(pContent)' % enum, count=1)
    replace(fvb, '(static_cast<const u16*>(pContent))[0]', 'p2_read_big<u16>(pContent)', count=1)
    replace(fvb, '(static_cast<const u16*>(pContent))[1]', 'p2_read_big<u16>((const u8*)pContent + 2)', count=1)
    replace(fvb, 'TData(*(u32*)data)', 'TData(p2_read_big<u32>(data))', count=1)
    replace(fvb, 'TData(*(f32*)data)', 'TData(p2_read_big<f32>(data))', count=5)
    # The file stores the operand four bytes after the operation word; a host pointer member would sit at eight.
    replace(fvb, 'JStudio::fvb::data::TEComposite v = content->_00;', 'JStudio::fvb::data::TEComposite v = (JStudio::fvb::data::TEComposite)p2_read_big<u32>(content);', count=1)
    replace(fvb, 'pfvaRange(&content->_04)', 'pfvaRange((const u8*)content + 4)', count=1)
    replace(fvb, 'const f32* content = static_cast<const f32*>(data.mContent);', 'const P2Big<f32>* content = static_cast<const P2Big<f32>*>(data.mContent);', count=2)
    replace('include/JSystem/JStudio/fvb.h', 'f32 _00;    // _00\n\t\tu32 _04;    // _04\n\t\tf32 _08[0];', 'P2Big<f32> _00; // _00\n\t\tP2Big<u32> _04; // _04\n\t\tP2Big<f32> _08[0];', count=1)
    replace('include/JSystem/JStudio/fvb.h', 'u32 _00;    // _00\n\t\tf32 _04[0];', 'P2Big<u32> _00; // _00\n\t\tP2Big<f32> _04[0];', count=2)
    # Function-value tables point straight into FVB data.
    replace('include/JSystem/JStudio/functionvalue.h', '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"', count=1)
    replace('include/JSystem/JStudio/functionvalue.h', 'const f32*', 'const P2Big<f32>*', count=16)
    replace('src/JSystem/JStudio/functionvalue.cpp', 'const f32*', 'const P2Big<f32>*', count=9)
    obj = 'src/JSystem/JStudio/jstudio-object.cpp'
    replace(obj, 'setValueImmediate(*(f32*)value);', 'setValueImmediate(p2_read_big<f32>(value));', count=1)
    replace(obj, 'setValueTime(*(f32*)value);', 'setValueTime(p2_read_big<f32>(value));', count=1)
    replace(obj, 'getFunctionValue_index(*(u32*)value)', 'getFunctionValue_index(p2_read_big<u32>(value))', count=1)
    for path, pointer, count in (
        ('src/JSystem/JStudio_JStage/object-actor.cpp', 'p2', 7), ('src/JSystem/JStudio_JStage/object-actor.cpp', 'p3', 1),
        ('src/JSystem/JStudio_JStage/object.cpp', 'p2', 1), ('src/JSystem/JStudio_JStage/object.cpp', 'p3', 1),
        ('src/JSystem/JStudio_JStage/object-camera.cpp', 'p2', 3), ('src/JSystem/JStudio_JParticle/object-particle.cpp', 'p2', 2),
        ('src/JSystem/JStudio_JMessage/object-message.cpp', 'p2', 2)):
        replace(path, '*(u32*)%s' % pointer, 'p2_read_big<u32>(%s)' % pointer, count=count)
    replace('src/JSystem/JStudio_JParticle/object-particle.cpp', '*(f32*)p2', 'p2_read_big<f32>(p2)', count=2)
    # Paragraph payloads stay big-endian; these handlers read them as raw ints.
    replace('src/JSystem/JStudio_JParticle/object-particle.cpp', '_188 = *(int*)p2;', '_188 = p2_read_big<s32>(p2);')
    replace('src/JSystem/JStudio_JStage/object-light.cpp', 'switch (((int*)data)[0]) {', 'switch (p2_read_big<s32>(data)) {')
    replace('src/JSystem/JStudio_JStage/object-camera.cpp', '_110 = ((int*)p2)[0] != 0;', '_110 = p2_read_big<s32>(p2) != 0;')
    replace('src/JSystem/JStudio_JStage/object-camera.cpp', '_11C = ((int*)data)[0] != 0;', '_11C = p2_read_big<s32>(data) != 0;')
    replace('src/JSystem/JSupport/JSUMemoryStream.cpp', '(int)mObject', '(uintptr_t)mObject', count=1)
    replace('src/JSystem/JUtility/JUTDirectFile.cpp', '(u32)mBuffer', '(uintptr_t)mBuffer', count=1)
    replace('src/JSystem/JUtility/JUTGraphFifo.cpp', '(u32)mBase', '(uintptr_t)mBase', count=1)
    replace('src/JSystem/JUtility/JUTTexture.cpp', '(u32)mTexInfo +', '(uintptr_t)mTexInfo +', count=2)
    replace('src/JSystem/JUtility/JUTException.cpp', '(u32)end', '(uintptr_t)end', count=1)
    replace('src/JSystem/JUtility/JUTException.cpp', '(s32)begin', '(uintptr_t)begin', count=1)
    replace('src/JSystem/JUtility/JUTException.cpp', 'IS_POSITIVE(flt)', 'signbit(flt)', count=1)
    # This embedded BFN is authored as PowerPC words, but it is a byte resource,
    # just like a disc BFN. Preserve all bytes (including glyph texels) rather
    # than letting the host lay out those integer literals in little-endian order.
    embedded_font = destination / 'src/JSystem/JUtility/JUTFontData_Ascfont_fix12.cpp'
    embedded_text = embedded_font.read_text()
    font_words = re.findall(r'0x[0-9A-Fa-f]{8}', embedded_text)
    if len(font_words) * 4 != 0x4160: raise RuntimeError('Embedded font word count drift')
    embedded_text = embedded_text.replace('const int JUTResFONT_Ascfont_fix12', 'const unsigned char JUTResFONT_Ascfont_fix12')
    embedded_text = re.sub(r'0x[0-9A-Fa-f]{8}',
        lambda m: ', '.join(f'0x{byte:02x}' for byte in int(m[0], 16).to_bytes(4, 'big')), embedded_text)
    embedded_font.write_text(embedded_text)
    replace('include/JSystem/JUtility/JUTResFONT_Ascfont_fix12.h', 'const int JUTResFONT_Ascfont_fix12', 'const unsigned char JUTResFONT_Ascfont_fix12', count=1)
    replace('src/plugProjectEbisawaU/ebiP2TitlePikmin.cpp', 'case TITLECREATURE_NULL:', 'case (u32)TITLECREATURE_NULL:', count=2)
    replace('src/plugProjectKandoU/vsGameSection.cpp', 'Switch_0, nullptr, nullptr, JKRDvdRipper::ALLOC_DIR_BOTTOM', 'Switch_0, 0, nullptr, JKRDvdRipper::ALLOC_DIR_BOTTOM', count=2)
    replace('src/plugProjectKandoU/vsGameSection.cpp', 'JKRDvdRipper::ALLOC_DIR_BOTTOM, nullptr, nullptr, nullptr', 'JKRDvdRipper::ALLOC_DIR_BOTTOM, 0, nullptr, nullptr', count=1)
    replace('src/plugProjectKandoU/vsGameSection.cpp', 'JKRDvdRipper::ALLOC_DIR_BOTTOM,\n\t                                         nullptr, nullptr, nullptr', 'JKRDvdRipper::ALLOC_DIR_BOTTOM,\n\t                                         0, nullptr, nullptr', count=1)
    replace('src/plugProjectKandoU/vsGameSection.cpp', '0.2f <= FABS(spawnFactor) < 0.4f', '(0.2f <= FABS(spawnFactor)) < 0.4f', count=1)
    replace('src/plugProjectKandoU/vsGameSection.cpp', '0.4f <= FABS(spawnFactor) < 0.8f', '(0.4f <= FABS(spawnFactor)) < 0.8f', count=1)
    replace('src/sysGCU/titleSection.cpp', '"code size           %dKB\\n", ((int)JKRHeap::getCodeEnd() - (int)JKRHeap::getCodeStart()) / 1024', '"code size           %zuKB\\n", ((uintptr_t)JKRHeap::getCodeEnd() - (uintptr_t)JKRHeap::getCodeStart()) / 1024', count=1)
    # JSU typed reads consume big-endian file scalars; raw reads stay byte-exact.
    replace("include/JSystem/JSupport/JSUStream.h", '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"')
    replace("include/JSystem/JSupport/JSUStream.h", "inline s32 read(s16& val) { return read(&val, sizeof(val)); }",
            "inline s32 read(s16& val) { val = 0; s32 n = read(&val, sizeof(val)); val = p2_big_endian(val); return n; }")
    file = destination / "include/JSystem/JSupport/JSUStream.h"
    text = file.read_text()
    for name, kind in (("readS16", "s16"), ("readS16ToFloat", "s16"), ("readU16", "u16"),
                       ("readU16ToFloat", "u16"), ("readU32", "u32"), ("readS32", "s32")):
        pattern = rf"(inline \w+ {name}\(\)\n\t\{{).*?(\n\t\}})"
        replacement = rf"\1\n\t\t{kind} value = 0; read(&value, sizeof(value)); return p2_big_endian(value);\2"
        text, count = re.subn(pattern, replacement, text, flags=re.S)
        if count != 1: raise RuntimeError(f"JSU endian patch drift: {name}")
    file.write_text(text)
    replace("include/JSystem/JSupport/JSUStream.h", "int write(s16 val) { return write(&val, sizeof(val)); }",
            "int write(s16 val) { val = p2_big_endian(val); return write(&val, sizeof(val)); }")
    replace("src/JSystem/JSupport/JSUInputStream.cpp", "int len  = readData(str, val);", "val = p2_big_endian(val);\n\tint len  = readData(str, val);")
    replace("src/JSystem/JSupport/JSUOutputStream.cpp", "u16 val = len;", "u16 val = p2_big_endian((u16)len);")
    replace("src/JSystem/JSupport/JSUMemoryStream.cpp", "if (mPosition + length > mLength)",
            "if (length <= 0) return 0;\n\tif (length > mLength - mPosition)")
    replace("src/JSystem/JSupport/JSUMemoryStream.cpp", "u32 originalPosition = mPosition;",
            "s32 originalPosition = mPosition;\n\tint64_t position = mPosition;")
    replace("src/JSystem/JSupport/JSUMemoryStream.cpp", "mPosition = offset;", "position = offset;")
    replace("src/JSystem/JSupport/JSUMemoryStream.cpp", "mPosition = mLength - offset;", "position = (int64_t)mLength - offset;")
    replace("src/JSystem/JSupport/JSUMemoryStream.cpp", "mPosition += offset;", "position += offset;")
    replace("src/JSystem/JSupport/JSUMemoryStream.cpp", "if (0 > mPosition)",
            "mPosition = position < 0 ? 0 : position > mLength ? mLength : (s32)position;\n\tif (0 > mPosition)")
    # This regional #if splits an if-statement, so the lexical scope pass cannot parse it.
    replace("src/plugProjectEbisawaU/ebiMainTitleMgr.cpp", "case MainMenu:", "case MainMenu: {")
    replace("src/plugProjectEbisawaU/ebiMainTitleMgr.cpp", "\tcase Exiting:", "\t}\n\tcase Exiting:")
    # Paired-single stores previously left their outputs uninitialized on Clang.
    file = destination / "include/Dolphin/OS/OSFastCast.h"
    text = file.read_text()
    for kind, lower, upper in (("s16", -32768, 32767), ("s8", -128, 127), ("u8", 0, 255)):
        pattern = rf"static inline {kind} __OSf32to{kind}\(register f32 inF\)\n\{{.*?^\}}"
        body = (f"static inline {kind} __OSf32to{kind}(f32 inF)\n{{\n"
                f"    if (isnan(inF)) return 0;\n    if (inF <= {lower}.0f) return {lower};\n"
                f"    if (inF >= {upper}.0f) return {upper};\n    return ({kind})inF;\n}}")
        text, count = re.subn(pattern, lambda m: body, text, flags=re.M | re.S)
        if count != 1: raise RuntimeError(f"OS fast-cast patch drift: {kind}")
    file.write_text(text)
    # Absolute-address SDK symbols need one platform definition, not one per translation unit.
    replace('include/Dolphin/hw_regs.h', 'vu16 __VIRegs[59] AT_ADDRESS(0xCC002000);', 'extern vu16 __VIRegs[59] AT_ADDRESS(0xCC002000);')
    replace('include/Dolphin/hw_regs.h', 'vu32 __PIRegs[12] AT_ADDRESS(0xCC003000);', 'extern vu32 __PIRegs[12] AT_ADDRESS(0xCC003000);')
    replace('include/Dolphin/hw_regs.h', 'vu16 __MEMRegs[64] AT_ADDRESS(0xCC004000);', 'extern vu16 __MEMRegs[64] AT_ADDRESS(0xCC004000);')
    replace('include/Dolphin/hw_regs.h', 'vu16 __DSPRegs[32] AT_ADDRESS(0xCC005000);', 'extern vu16 __DSPRegs[32] AT_ADDRESS(0xCC005000);')
    replace('include/Dolphin/hw_regs.h', 'vu32 __DIRegs[16] AT_ADDRESS(0xCC006000);', 'extern vu32 __DIRegs[16] AT_ADDRESS(0xCC006000);')
    replace('include/Dolphin/hw_regs.h', 'vu32 __SIRegs[64] AT_ADDRESS(0xCC006400);', 'extern vu32 __SIRegs[64] AT_ADDRESS(0xCC006400);')
    replace('include/Dolphin/hw_regs.h', 'vu32 __EXIRegs[16] AT_ADDRESS(0xCC006800);', 'extern vu32 __EXIRegs[16] AT_ADDRESS(0xCC006800);')
    replace('include/Dolphin/hw_regs.h', 'vu32 __AIRegs[8] AT_ADDRESS(0xCC006C00);', 'extern vu32 __AIRegs[8] AT_ADDRESS(0xCC006C00);')
    replace('include/Dolphin/exi.h', 's32 __EXIProbeStartTime[2] AT_ADDRESS(OS_BASE_CACHED | 0x30C0);', 'extern s32 __EXIProbeStartTime[2] AT_ADDRESS(OS_BASE_CACHED | 0x30C0);')
    replace('include/Dolphin/os.h', 'u16 __OSWirelessPadFixMode AT_ADDRESS(OS_BASE_CACHED | 0x30E0);', 'extern u16 __OSWirelessPadFixMode AT_ADDRESS(OS_BASE_CACHED | 0x30E0);')
    replace('include/Dolphin/os.h', 'u8 GameChoice AT_ADDRESS(OS_BASE_CACHED | 0x30E3);', 'extern u8 GameChoice AT_ADDRESS(OS_BASE_CACHED | 0x30E3);')
    replace('include/Dolphin/os.h', 'volatile int __OSTVMode AT_ADDRESS(OS_BASE_CACHED | 0xCC);', 'extern volatile int __OSTVMode AT_ADDRESS(OS_BASE_CACHED | 0xCC);')
    replace('include/Dolphin/OS/OSUtil.h', 'u32 __OSBusClock AT_ADDRESS(OS_BASE_CACHED | 0x00F8);', 'extern u32 __OSBusClock AT_ADDRESS(OS_BASE_CACHED | 0x00F8);')
    replace('include/Dolphin/OS/OSUtil.h', 'u32 __OSCoreClock AT_ADDRESS(OS_BASE_CACHED | 0x00FC);', 'extern u32 __OSCoreClock AT_ADDRESS(OS_BASE_CACHED | 0x00FC);')
    replace('include/Dolphin/OS/OSUtil.h', 'vu16 __OSDeviceCode AT_ADDRESS(OS_BASE_CACHED | 0x30E6);', 'extern vu16 __OSDeviceCode AT_ADDRESS(OS_BASE_CACHED | 0x30E6);')
    replace('include/Dolphin/OS/OSThread.h', 'volatile OSContext* __OSCurrentContext AT_ADDRESS((u32)OSPhysicalToCached(0x00D4));', 'extern volatile OSContext* __OSCurrentContext AT_ADDRESS((u32)OSPhysicalToCached(0x00D4));')
    replace('include/Dolphin/OS/OSThread.h', 'volatile OSContext* __OSFPUContext AT_ADDRESS((u32)OSPhysicalToCached(0x00D8));', 'extern volatile OSContext* __OSFPUContext AT_ADDRESS((u32)OSPhysicalToCached(0x00D8));')
    replace('include/Dolphin/OS/OSThread.h', 'OSThreadQueue __OSActiveThreadQueue AT_ADDRESS((u32)OSPhysicalToCached(0x00DC));', 'extern OSThreadQueue __OSActiveThreadQueue AT_ADDRESS((u32)OSPhysicalToCached(0x00DC));')
    replace('include/Dolphin/OS/OSThread.h', 'OSThread* __OSCurrentThread AT_ADDRESS((u32)OSPhysicalToCached(0x00E4));', 'extern OSThread* __OSCurrentThread AT_ADDRESS((u32)OSPhysicalToCached(0x00E4));')
    replace('include/Dolphin/OS/OSInterrupt.h', 'volatile OSInterruptMask __OSPriorInterruptMask AT_ADDRESS((u32)OSPhysicalToCached(0x00C4));', 'extern volatile OSInterruptMask __OSPriorInterruptMask AT_ADDRESS((u32)OSPhysicalToCached(0x00C4));')
    replace('include/Dolphin/OS/OSInterrupt.h', 'volatile OSInterruptMask __OSCurrentInterruptMask AT_ADDRESS((u32)OSPhysicalToCached(0x00C8));', 'extern volatile OSInterruptMask __OSCurrentInterruptMask AT_ADDRESS((u32)OSPhysicalToCached(0x00C8));')
    # Audited pointer-free disk records. Keep native runtime fields unchanged.
    for path, names in (("include/JSystem/J3D/J3DFileBlock.h", ("J3DFileHeader", "J3DFileBlockBase")),
                        ("include/JSystem/J2D/J2DPane.h", ("J2DScrnBlockHeader", "J2DScreenInfoBlock", "J2DPaneExBlock", "J2DTextBoxBlock", "J2DScrnBlockPictureParameter")),
                        ("include/JSystem/J2D/J2DWindow.h", ("J2DWindowData",))):
        file = destination / path
        text = '#include "p2_endian.h"\n' + file.read_text()
        for name in names:
            pattern = rf"(struct {name}[^;{{]*\{{)(.*?)(^\}};)"
            def fields(match):
                body = re.sub(r"(?m)^(\s*)(u16|s16|u32|s32|u64|s64|int|f32)(\s+\w+(?:\[\d+\])?;)",
                              r"\1P2Big<\2>\3", match[2])
                return match[1] + body + match[3]
            text, count = re.subn(pattern, fields, text, flags=re.M | re.S)
            if count != 1: raise RuntimeError(f"Disk record patch drift: {name}")
        file.write_text(text)
    # Packed GX colors are numeric RRGGBBAA, independent of native byte order.
    replace("include/JSystem/JUtility/TColor.h", "return GXCOLOR_AS_U32(*this);", "return (u32(r)<<24) | (u32(g)<<16) | (u32(b)<<8) | u32(a);")
    replace("include/JSystem/JUtility/TColor.h", "GXCOLOR_AS_U32(*this) = u32Color;", "set(u8(u32Color>>24),u8(u32Color>>16),u8(u32Color>>8),u8(u32Color));")
    replace("src/JSystem/J2D/J2DPicture.cpp", "GXCOLOR_AS_U32(trailer.mCornerColor[i])", "u32(trailer.mCornerColor[i])")
    # Extended picture/window constructors also contain raw two-word headers.
    for path, arrays in (("J2DPictureEx.cpp", ("info", "nextInfo")),
                         ("J2DWindow.cpp", ("streamData", "newData", "startData")),
                         ("J2DWindowEx.cpp", ("uStack_88", "auStack_90"))):
        for array in arrays:
            scalar = "int" if array in ("newData", "uStack_88", "auStack_90") else "u32"
            replace("src/JSystem/J2D/" + path, f"{scalar} {array}[2];", f"P2Big<{scalar}> {array}[2];")
    for path in ("J2DPicture.cpp", "J2DPictureEx.cpp"):
        replace("src/JSystem/J2D/" + path,
                "input->read(&trailer, sizeof(J2DScrnBlockPictureParameter));",
                "input->read(&trailer, sizeof(J2DScrnBlockPictureParameter));\n"
                "\tfor (int i = 0; i < 4; ++i) {\n"
                "\t\ttrailer.mTexCoords[i].x = p2_big_endian(trailer.mTexCoords[i].x);\n"
                "\t\ttrailer.mTexCoords[i].y = p2_big_endian(trailer.mTexCoords[i].y);\n\t}")
    replace("src/JSystem/J2D/J2DScreen.cpp",
            "\t\tinput->peek(&header, sizeof(J2DScrnBlockHeader));",
            "\t\tconst int blockStart = input->getPosition();\n"
            "\t\tif (input->peek(&header, sizeof(header)) != sizeof(header) ||\n"
            "\t\t    header.mBlockLength < sizeof(header) ||\n"
            "\t\t    header.mBlockLength > input->getLength() - blockStart)\n"
            "\t\t\tOSPanic(__FILE__, __LINE__, \"Invalid J2D block at %d: %08x size %d\",\n"
            "\t\t\t        blockStart, (u32)header.mBloBlockType, (int)header.mBlockLength);")
    replace("src/JSystem/J2D/J2DPane.cpp", "input->read(&tag, 4);", "tag = input->readU32();")
    replace('include/Game/gameGenerator.h', 'doEvent(u32)', 'doEvent(uintptr_t)', count=3)
    replace('src/plugProjectKandoU/genPellet.cpp', 'GenPellet::doEvent(u32 flag)', 'GenPellet::doEvent(uintptr_t flag)', count=1)
    replace('src/plugProjectKandoU/genPellet.cpp', '(int)&mPelType', '(uintptr_t)&mPelType', count=1)
    replace('src/plugProjectKandoU/genItem.cpp', 'GenItem::doEvent(u32 idx)', 'GenItem::doEvent(uintptr_t idx)', count=1)
    replace('include/JSystem/JKernel/JKRArchive.h', 'JKRArchive(s32 entryNum', 'JKRArchive(intptr_t entryNum', count=1)
    replace('include/JSystem/JKernel/JKRArchive.h', 'check_mount_already(s32 entryNum', 'check_mount_already(intptr_t entryNum', count=2)
    replace('include/JSystem/JKernel/JKRArchive.h', 'int mEntryNum;', 'intptr_t mEntryNum;', count=1)
    replace('src/JSystem/JKernel/JKRArchivePri.cpp', 'JKRArchive::JKRArchive(s32 entryNum', 'JKRArchive::JKRArchive(intptr_t entryNum', count=1)
    replace('src/JSystem/JKernel/JKRArchivePub.cpp', 'check_mount_already(s32 entryNum', 'check_mount_already(intptr_t entryNum', count=2)
    replace('src/JSystem/JKernel/JKRArchivePub.cpp', '(s32)mem', '(intptr_t)mem', count=1)
    replace('src/JSystem/JKernel/JKRArchivePub.cpp', 'u32 JKRArchive::readResource', 'size_t JKRArchive::readResource', count=3)
    replace('src/JSystem/JKernel/JKRMemArchive.cpp', '(s32)mem', '(intptr_t)mem', count=1)
    replace('src/JSystem/JKernel/JKRMemArchive.cpp', '(u32)mHeader', '(uintptr_t)mHeader', count=2)
    replace('src/JSystem/JKernel/JKRFileLoader.cpp', '__lower_map[ch & 0xFF]', 'tolower((unsigned char)ch)', count=1)
    replace('include/JSystem/JKernel/JKRAram.h', 'extern OSMutex decompMutex;', '// Each decompressor owns its file-local mutex.', count=1)
    replace('src/JSystem/JKernel/JKRAramArchive.cpp', '(u32)decompBuf', '(uintptr_t)decompBuf', count=1)
    replace('src/JSystem/JKernel/JKRAramArchive.cpp', '(u32)buf', '(uintptr_t)buf', count=1)
    replace('src/JSystem/JKernel/JKRFileCache.cpp', 'size_t JKRFileCache::readRelResource(void* p1, u32 p2, const char* p3)\n{\n\t// UNUSED FUNCTION\n}', 'u32 JKRFileCache::readRelResource(void* p1, u32 p2, const char* p3)\n{\n\treturn readResource(p1, p2, p3);\n}', count=1)
    replace('src/JSystem/JKernel/JKRFileCache.cpp', 'return readResource(resourceBuffer, bufferSize, path);\n\t}\n\treturn nullptr;', 'return readResource(resourceBuffer, bufferSize, path);\n\t}\n\treturn 0;', count=1)
    replace('include/JSystem/JGadget/vector.h', 'struct TVector_pointer_void :', 'template <> TVector<void*, TAllocator<void*> >::~TVector();\n\nstruct TVector_pointer_void :', count=1)
    # Propagate loader and text-color results; falling off a bool function is undefined on Clang.
    replace("include/JSystem/J2D/J2DManage.h", '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"')
    replace("include/JSystem/J2D/J2DManage.h", "u16 mCount;", "P2Big<u16> mCount;")
    replace("include/JSystem/J2D/J2DManage.h", "u16 mOffsets[];", "P2Big<u16> mOffsets[];")
    replace("src/JSystem/J2D/J2DManage.cpp", "ref[1]", "static_cast<u8>(ref[1])", 4)
    replace("src/JSystem/J2D/J2DScreen.cpp", '#include "JSystem/J2D/J2DAnm.h"', '#include "JSystem/J2D/J2DAnm.h"\n#include "p2_layout.h"')
    replace("src/JSystem/J2D/J2DScreen.cpp", "input->read(blank, 8);",
            "blank[0] = input->readU32();\n\tblank[1] = input->readU32();\n"
            "\tif (initialPosition < 0 || initialPosition > input->getLength() || blank[1] < 0x68 ||\n"
            "\t    blank[1] > static_cast<u32>(input->getLength() - initialPosition)) return false;")
    replace("src/JSystem/J2D/J2DScreen.cpp", "input->seek(initialPosition + size2, SEEK_SET);",
            "if (initialPosition < 0 || initialPosition > input->getLength() || size1 < 16 || size2 < 16 ||\n"
            "\t    size2 > size1 || size1 > input->getLength() - initialPosition) return nullptr;\n"
            "\tinput->seek(initialPosition + size2, SEEK_SET);")
    replace("src/JSystem/J2D/J2DScreen.cpp", "input->read(data, size1);",
            "if (input->read(data, size1) != size1 || !p2_validate_2d_references(data, size1)) {\n"
            "\t\t\tdelete[] data;\n\t\t\treturn nullptr;\n\t\t}")
    replace("src/JSystem/J2D/J2DScreen.cpp", "\tprivate_set(stream, flags, nullptr);", "\treturn private_set(stream, flags, nullptr);")
    replace("src/JSystem/J2D/J2DTextBoxEx.cpp", "\tsetBlackWhite(black, currWhite);", "\treturn setBlackWhite(black, currWhite);")
    replace("src/JSystem/J2D/J2DTextBoxEx.cpp", "\tsetBlackWhite(currBlack, white);", "\treturn setBlackWhite(currBlack, white);")
    # Native heap addresses and in-memory allocation headers.
    replace('src/JSystem/JKernel/JKRHeap.cpp', '(u32)ram_end', '(uintptr_t)ram_end', count=1)
    replace('src/JSystem/JKernel/JKRHeap.cpp', '(u32)ram_start', '(uintptr_t)ram_start', count=1)
    replace('src/JSystem/JKernel/JKRHeap.cpp', '(u32)getMaxFreeBlock()', '(uintptr_t)getMaxFreeBlock()', count=1)
    replace('src/JSystem/JKernel/JKRHeap.cpp', '(u32)memory', '(uintptr_t)memory', count=2)
    replace('src/JSystem/JKernel/JKRHeap.cpp', '(u32)begin', '(uintptr_t)begin', count=1)
    replace('src/JSystem/JKernel/JKRHeap.cpp', '(u32)end', '(uintptr_t)end', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', '(u32)block->getContent()', '(uintptr_t)block->getContent()', count=4)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', '(u32)foundBlock', '(uintptr_t)foundBlock', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', '(u32)(block + 1)', '(uintptr_t)(block + 1)', count=2)
    replace('src/JSystem/JKernel/JKRSolidHeap.cpp', '(u32)mStartAddress', '(uintptr_t)mStartAddress', count=4)
    replace('src/JSystem/JKernel/JKRSolidHeap.cpp', '(u32)mSolidHead', '(uintptr_t)mSolidHead', count=6)
    replace('src/JSystem/JKernel/JKRSolidHeap.cpp', '(u32)mSolidTail', '(uintptr_t)mSolidTail', count=7)
    replace('src/JSystem/JKernel/JKRSolidHeap.cpp', '(u32)mEndAddress', '(uintptr_t)mEndAddress', count=4)
    replace('src/JSystem/JKernel/JKRSolidHeap.cpp', '(u32)alignedStart', '(uintptr_t)alignedStart', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', '(u32)block +', '(uintptr_t)block +', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', '(u32)block *', '(uintptr_t)block *', count=2)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', '(u32)block->mNext', '(uintptr_t)block->mNext', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', '(u32)this', '(uintptr_t)this', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', '(u32)next -', '(uintptr_t)next -', count=1)
    replace('src/JSystem/JKernel/JKRSolidHeap.cpp', '(u32)this', '(uintptr_t)this', count=1)
    replace('src/JSystem/JKernel/JKRHeap.cpp', 'u32 maxFreeBlock =', 'uintptr_t maxFreeBlock =', count=1)
    replace('src/JSystem/JKernel/JKRHeap.cpp', 'dispose_subroutine(u32 begin, u32 end)', 'dispose_subroutine(uintptr_t begin, uintptr_t end)', count=1)
    replace('include/JSystem/JKernel/JKRHeap.h', 'dispose_subroutine(u32 begin, u32 end)', 'dispose_subroutine(uintptr_t begin, uintptr_t end)', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', 'u32 start;', 'uintptr_t start;', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', 'u32 endAddr', 'uintptr_t endAddr', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', 'u32 nextAddr', 'uintptr_t nextAddr', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', 'p2 - 0x10', 'p2 - sizeof(CMemBlock)', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', 'if (alignedSize < 0xa0)', 'if (alignedSize < expHeapSize + sizeof(CMemBlock) + alignof(CMemBlock))', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', 'size        = ALIGN_NEXT(size, 4);', 'size        = ALIGN_NEXT(size, alignof(CMemBlock));', count=1)
    replace('src/JSystem/JKernel/JKRExpHeap.cpp', 'size = ALIGN_NEXT(size, 4);', 'size = ALIGN_NEXT(size, alignof(CMemBlock));', count=1)
    replace('src/JSystem/JKernel/JKRSolidHeap.cpp', 'u32 alignedStart', 'uintptr_t alignedStart', count=2)
    replace('src/JSystem/JKernel/JKRSolidHeap.cpp', '& ~(alignment - 1)', '& ~((uintptr_t)alignment - 1)', count=1)
    replace('include/Dolphin/OS/OSUtil.h', '(((u32)(x) + 0x1F) & ~(0x1F))', '(((uintptr_t)(x) + 0x1F) & ~(uintptr_t)0x1F)', count=1)
    replace('include/Dolphin/OS/OSUtil.h', '(((u32)(x)) & ~(0x1F))', '(((uintptr_t)(x)) & ~(uintptr_t)0x1F)', count=1)
    # Native MEM1 translation preserves the 32-bit GPU/ARAM address space.
    file = destination / "include/Dolphin/OS/OSUtil.h"
    text = '#include "p2_memory.h"\n' + file.read_text()
    for macro, parameter, body in (
        ("OSPhysicalToCached", "paddr", "p2_physical_to_host((u32)(paddr))"),
        ("OSPhysicalToUncached", "paddr", "p2_physical_to_host((u32)(paddr))"),
        ("OSCachedToPhysical", "caddr", "p2_host_to_physical(caddr)"),
        ("OSUncachedToPhysical", "ucaddr", "p2_host_to_physical(ucaddr)"),
        ("OSCachedToUncached", "caddr", "((void*)(caddr))"),
        ("OSUncachedToCached", "ucaddr", "((void*)(ucaddr))")):
        text, count = re.subn(rf"(?m)^#define {macro}\([^\n]*", f"#define {macro}({parameter}) {body}", text)
        if count != 1: raise RuntimeError(f"OS address patch drift: {macro}")
    file.write_text(text)
    file = destination / "src/JSystem/JKernel/JKRHeap.cpp"
    text = '#include "p2_game_alloc.h"\n' + file.read_text()
    def new_operator(match):
        array, arguments = match[1], match[2]
        alignment = "p3" if "p3" in arguments else "p2" if "p2" in arguments else "16"
        heap = "heap" if "JKRHeap*" in arguments else "nullptr"
        return f"void* operator new{array}(size_t byteCount{arguments})\n{{\n\treturn p2_game_alloc(byteCount, {alignment}, {heap});\n}}"
    text, count = re.subn(r"void\* operator new(\[\]|)\(u32 byteCount([^)]*)\)\n\{.*?^\}", new_operator, text, flags=re.M | re.S)
    if count != 6: raise RuntimeError("Native new operator patch drift")
    text = text.replace("JKRHeap::free(memory, nullptr);", "p2_game_free(memory);")
    text = text.replace("void operator delete(void* memory)", "void operator delete(void* memory) noexcept")
    text = text.replace("void operator delete[](void* memory)", "void operator delete[](void* memory) noexcept")
    file.write_text(text)
    replace("src/JSystem/JKernel/JKRHeap.cpp", "mTree.getParent()->removeChild(&mTree);",
            "p2_destroy_mutex(&mMutex);\n\tif (mTree.getParent()) mTree.getParent()->removeChild(&mTree);")
    replace("src/JSystem/JKernel/JKRHeap.cpp", "arenaStart = OSInitAlloc(arenaLo, arenaHi, maxHeaps);",
            "arenaStart = OSInitAlloc(arenaLo, arenaHi, maxHeaps);\n\tif (!arenaStart) return false;")
    replace("src/JSystem/JKernel/JKRHeap.cpp", "return ~(alignment - 1) & (getFreeSize() - ptrOffset);",
            "return getFreeSize() > ptrOffset ? (~(alignment - 1) & (getFreeSize() - ptrOffset)) : 0;")
    replace("src/JSystem/JKernel/JKRHeap.cpp", "u32 ptrOffset    = (alignment - 1) & alignment - (maxFreeBlock & 0xf);",
            "if (alignment <= 0 || (alignment & (alignment - 1))) return 0;\n"
            "\tif (getHeapType() == 'EXPH') maxFreeBlock += sizeof(JKRExpHeap::CMemBlock);\n"
            "\tu32 ptrOffset = (uintptr_t(0) - maxFreeBlock) & uintptr_t(alignment - 1);")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "int JKRExpHeap::do_resize(void* ptr, u32 size)\n{",
            "int JKRExpHeap::do_resize(void* ptr, u32 size)\n{\n\tif (size > mHeapSize) return -1;")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "JKRExpHeap* heap = nullptr;",
            "if (sRootHeap) return static_cast<JKRExpHeap*>(sRootHeap);\n\tJKRExpHeap* heap = nullptr;")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "initArena((char**)&memory, &memorySize, maxHeaps);",
            "if (!initArena((char**)&memory, &memorySize, maxHeaps)) return nullptr;")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "OSLockMutex(&mMutex);\n\tif (byteCount < 4)",
            "if (byteCount > mHeapSize || padding == INT_MIN) return nullptr;\n"
            "\tint magnitude = padding < 0 ? -padding : padding;\n"
            "\tif (magnitude && (magnitude & (magnitude - 1))) return nullptr;\n"
            "\tif (magnitude < alignof(CMemBlock)) padding = padding < 0 ? -int(alignof(CMemBlock)) : int(alignof(CMemBlock));\n"
            "\tOSLockMutex(&mMutex);\n\tif (byteCount < 4)")
    # Console CMemBlock is 16 bytes, so block content after a 16-aligned header was 16-aligned; the
    # 64-bit header is 24. Without this a 16-aligned request for a whole free block (Challenge/VS title
    # makeExpHeap(getFreeSize())) needs padding it cannot have and fails.
    replace("include/JSystem/JKernel/JKRHeap.h", "struct CMemBlock {", "struct alignas(16) CMemBlock {", count=1)
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "size                  = ALIGN_NEXT(size, 4);", "size                  = ALIGN_NEXT(size, alignof(CMemBlock));")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "u32 size2             = ALIGN_NEXT(size, 4);", "u32 size2             = ALIGN_NEXT(size, alignof(CMemBlock));")
    # P2_HEAP_CHECK=<tick>: from that input tick on, after every alloc, free
    # and resize, check that free and used blocks never overlap; on the first
    # overlap print both blocks and the operation that caused it.
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "\tOSUnlockMutex(&mMutex);\n\treturn mem;\n}",
            "\tp2HeapCheck(this, mHead, mHeadUsedList, mStartAddress, mEndAddress, \"alloc\", mem, byteCount);\n\tOSUnlockMutex(&mMutex);\n\treturn mem;\n}")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "\t\tif (block != nullptr) {\n\t\t\tblock->free(this);\n\t\t}\n\t}\n\tOSUnlockMutex(&mMutex);",
            "\t\tif (block != nullptr) {\n\t\t\tblock->free(this);\n\t\t}\n\t}\n\tp2HeapCheck(this, mHead, mHeadUsedList, mStartAddress, mEndAddress, \"free\", p1, 0);\n\tOSUnlockMutex(&mMutex);")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "\tunlock();\n\treturn block->mAllocatedSpace;\n}",
            "\tp2HeapCheck(this, mHead, mHeadUsedList, mStartAddress, mEndAddress, \"resize\", ptr, size);\n\tunlock();\n\treturn block->mAllocatedSpace;\n}")
    # P2_HEAP_GROUP_TRACE=1: log allocations made under a group ID that another
    # thread set (the resource manager's per-load groups are heap-global).
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "\tu8 oldGroupID   = mCurrentGroupID;\n\tmCurrentGroupID = groupID;",
            "\tu8 oldGroupID   = mCurrentGroupID;\n\tmCurrentGroupID = groupID;\n\tp2HeapGroupSet(this);")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "\tOSLockMutex(&mMutex);\n\tif (byteCount < 4) {",
            "\tOSLockMutex(&mMutex);\n\tp2HeapGroupUse(this, mCurrentGroupID, byteCount);\n\tif (byteCount < 4) {")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "\t\tCMemBlock* block = CMemBlock::getHeapBlock(p1);\n\t\tif (block != nullptr) {",
            "\t\tCMemBlock* block = CMemBlock::getHeapBlock(p1);\n\t\tif (block) p2HeapLargeOp(this, \"free\", p1, block->mAllocatedSpace, block->mGroupID);\n\t\tif (block != nullptr) {")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "\tp2HeapCheck(this, mHead, mHeadUsedList, mStartAddress, mEndAddress, \"alloc\", mem, byteCount);",
            "\tif (mem) p2HeapLargeOp(this, \"alloc\", mem, int(byteCount), mCurrentGroupID);\n\tp2HeapCheck(this, mHead, mHeadUsedList, mStartAddress, mEndAddress, \"alloc\", mem, byteCount);")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "void JKRExpHeap::do_freeAll()\n{\n\tlock();",
            "void JKRExpHeap::do_freeAll()\n{\n\tlock();\n\tp2HeapLargeOp(this, \"freeAll\", mStartAddress, int(mEndAddress - mStartAddress), 0);")
    replace("src/JSystem/JKernel/JKRExpHeap.cpp", "\twhile (block != nullptr) {\n\t\tif (block->mGroupID == groupID) {\n\t\t\tdispose(block + 1, block->mAllocatedSpace);",
            "\twhile (block != nullptr) {\n\t\tif (block->mGroupID == groupID) {\n\t\t\tp2HeapLargeOp(this, \"freeGroup\", block + 1, block->mAllocatedSpace, groupID);\n\t\t\tdispose(block + 1, block->mAllocatedSpace);")
    file = destination / "src/JSystem/JKernel/JKRExpHeap.cpp"
    text = file.read_text()
    anchor = "JKRExpHeap::~JKRExpHeap()"
    if text.count(anchor) != 1: raise RuntimeError("Heap check patch drift")
    file.write_text(text.replace(anchor, r'''#include <algorithm>
#include <execinfo.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
extern "C" uint32_t p2_tick_now; // input tick (src/panic.cpp), set by record/replay
namespace {
struct P2HeapOp { void* ptr; u32 size; bool freed; void* frames[10]; int depth; };
P2HeapOp sHeapOps[1 << 16];
u32 sHeapOpCount;
struct P2HeapSpan { uintptr_t begin, end; JKRExpHeap::CMemBlock* block; bool used; };
P2HeapSpan sHeapSpans[1 << 17];
long p2HeapCheckFrom() {
	static const long from = [] { const char* v = std::getenv("P2_HEAP_CHECK"); return v ? std::atol(v) : -1L; }();
	return from;
}
void p2HeapCheck(JKRExpHeap* heap, JKRExpHeap::CMemBlock* freeList, JKRExpHeap::CMemBlock* usedList, u8* start, u8* end,
                 const char* what, void* ptr, u32 size) {
	const long from = p2HeapCheckFrom();
	if (from < 0) return;
	if (ptr) {
		P2HeapOp& op = sHeapOps[sHeapOpCount++ & 0xFFFF];
		op.ptr = ptr; op.size = size; op.freed = what[0] == 'f';
		op.depth = backtrace(op.frames, 10);
	}
	const u32 tick = p2_tick_now;
	if (long(tick) < from) return;
	u32 n = 0;
	const char* problem = nullptr;
	JKRExpHeap::CMemBlock* holder = nullptr;
	for (int used = 0; used < 2 && !problem; ++used)
		for (JKRExpHeap::CMemBlock *b = used ? usedList : freeList, *prev = nullptr; b; prev = b, b = b->mNext) {
			if ((u8*)b < start || (u8*)b >= end || n == (1 << 17)) { problem = "block outside heap"; holder = prev; break; }
			if (!used && b->mAllocatedSpace < 0) { problem = "bad free block header"; holder = prev; break; }
			uintptr_t begin = (uintptr_t)b - (used ? (b->mFlags & 0x7f) : 0);
			sHeapSpans[n++] = { begin, (uintptr_t)(b + 1) + u32(b->mAllocatedSpace), b, used != 0 };
		}
	std::sort(sHeapSpans, sHeapSpans + n, [](const P2HeapSpan& a, const P2HeapSpan& b) { return a.begin < b.begin; });
	u32 bad = 0;
	for (u32 i = 1; i < n && !problem; ++i)
		if (sHeapSpans[i].begin < sHeapSpans[i - 1].end) problem = "overlapping blocks", bad = i;
	if (!problem) return;
	std::fprintf(stderr, "*** HEAP BROKEN after %s(%p, 0x%x) at tick %u: %s, heap %p [%p,%p)\n", what, ptr, size,
	             tick, problem, heap, start, end);
	for (u32 i = bad ? bad - 1 : 0; bad && i <= bad; ++i)
		std::fprintf(stderr, "***   %s block %p span [%#lx,%#lx) size 0x%x flags 0x%x\n", sHeapSpans[i].used ? "used" : "free",
		             sHeapSpans[i].block, sHeapSpans[i].begin, sHeapSpans[i].end, sHeapSpans[i].block->mAllocatedSpace,
		             sHeapSpans[i].block->mFlags);
	if (holder) {
		u8* h = (u8*)holder;
		std::fprintf(stderr, "*** bad link %p stored in %s block %p (heap offset 0x%lx, size 0x%x, mPrev %p)\n",
		             (void*)holder->mNext, "free", holder, (long)(h - start), holder->mAllocatedSpace, (void*)holder->mPrev);
		for (u32 i = 0; i < 0x10000 && i < sHeapOpCount; ++i) {
			const P2HeapOp& op = sHeapOps[(sHeapOpCount - 1 - i) & 0xFFFF];
			u8* p = (u8*)op.ptr;
			if (h + sizeof(*holder) >= p - sizeof(*holder) && h < p + (op.size ? op.size : 0x10000)) {
				std::fprintf(stderr, "*** %u ops ago: %s %p size 0x%x\n", i, op.freed ? "free" : "alloc", op.ptr, op.size);
				backtrace_symbols_fd(op.frames + 2, op.depth > 2 ? op.depth - 2 : 0, 2);
			}
		}
	}
	void* frames[12];
	int depth = backtrace(frames, 12);
	backtrace_symbols_fd(frames, depth, 2);
	std::abort();
}
struct P2GroupSetter { JKRExpHeap* heap; pthread_t thread; };
P2GroupSetter sGroupSetters[64];
bool p2GroupTrace() { static const bool on = std::getenv("P2_HEAP_GROUP_TRACE") != nullptr; return on; }
void p2HeapGroupSet(JKRExpHeap* heap) {
	if (!p2GroupTrace()) return;
	static int logged;
	if (logged++ < 40) std::fprintf(stderr, "[HEAPGROUP] tick %u heap %p group %u thread %p\n", p2_tick_now, heap, heap->getCurrentGroupId(), (void*)pthread_self());
	for (auto& s : sGroupSetters) if (s.heap == heap || !s.heap) { s.heap = heap; s.thread = pthread_self(); return; }
}
long p2HeapLogFrom() {
	static const long from = [] { const char* v = std::getenv("P2_HEAP_LOG_FROM"); return v ? std::atol(v) : -1L; }();
	return from;
}
void p2HeapLargeOp(JKRExpHeap* heap, const char* what, void* ptr, int size, u8 group) {
	const bool logAll = p2HeapLogFrom() >= 0 && long(p2_tick_now) >= p2HeapLogFrom();
	if (!logAll && (!p2GroupTrace() || size < 0x40000)) return;
	std::fprintf(stderr, "[HEAPOP] tick %u %s heap %p block %p size 0x%x group %u thread %p\n", p2_tick_now, what, heap, ptr, size,
	             group, (void*)pthread_self());
	void* frames[10];
	int depth = backtrace(frames, 10);
	backtrace_symbols_fd(frames + 1, depth > 1 ? depth - 1 : 0, 2);
}
void p2HeapGroupUse(JKRExpHeap* heap, u8 group, u32 size) {
	static int reports;
	if (!p2GroupTrace() || !group || reports >= 40) return;
	for (auto& s : sGroupSetters) {
		if (s.heap != heap) continue;
		if (pthread_equal(s.thread, pthread_self())) return;
		++reports;
		std::fprintf(stderr, "*** GROUP RACE tick %u: heap %p alloc 0x%x under group %u set by another thread\n",
		             p2_tick_now, heap, size, group);
		void* frames[8];
		int depth = backtrace(frames, 8);
		backtrace_symbols_fd(frames + 2, depth > 2 ? depth - 2 : 0, 2);
		return;
	}
}
} // namespace

''' + anchor, 1))
    # Native archive storage preserves the original lookup and mount APIs.
    replace("include/JSystem/J3D/J3DVertexData.h", '#include "Dolphin/gx.h"', '#include "Dolphin/gx.h"\n#include "p2_game_alloc.h"')
    replace("include/JSystem/J3D/J3DVertexData.h", "J3DVertexData();",
            "J3DVertexData();\n\t~J3DVertexData() { p2_game_free(mNativeStorage); }\n"
            "\tvoid* mNativeStorage = nullptr;\n\tu32 mNativeArrayBytes[13] = {};\n\tbool mNativeArrayLittleEndian[13] = {};")
    file = destination / "src/JSystem/J3D/J3DJointFactory.cpp"
    text = '#include "p2_model_data.h"\n' + file.read_text()
    text = text.replace("J3DJoint* joint              = new J3DJoint;",
                        "const J3DJointInitData data = p2_decode_joint(mInitData + p2_big_endian(mIndexMap[jointIndex]));\n\tJ3DJoint* joint = new J3DJoint;")
    if text.count("mInitData[mIndexMap[jointIndex]]") != 6: raise RuntimeError("Joint initializer patch drift")
    text = text.replace("mInitData[mIndexMap[jointIndex]]", "data")
    file.write_text(text)
    replace("include/JSystem/JMath.h", "return ff25;",
            "const f32 duration = p5 - p2;\n\tif (duration <= 0.0f) return p3;\n"
            "\tconst f32 t = (p1 - p2) / duration, t2 = t*t, t3 = t2*t;\n"
            "\treturn (2*t3 - 3*t2 + 1)*p3 + (t3 - 2*t2 + t)*duration*p4\n"
            "\t     + (-2*t3 + 3*t2)*p6 + (t3 - t2)*duration*p7;")
    replace("include/JSystem/J3D/J3DAnmBase.h", "return fout;",
            "return JMAHermiteInterpolation(pp1, *pp2, *pp3, *pp4, *pp5, *pp6, *pp7);")
    replace("include/JSystem/J3D/J3DAnmTransform.h", '#include "types.h"', '#include "types.h"\n#include "p2_game_alloc.h"')
    replace("include/JSystem/J3D/J3DAnmTransform.h", ": mScaleVals(nullptr)", ": mNativeStorage(nullptr)\n\t    , mScaleVals(nullptr)")
    replace("include/JSystem/J3D/J3DAnmTransform.h", "virtual ~J3DAnmTransform() { }", "virtual ~J3DAnmTransform() { p2_game_free(mNativeStorage); }")
    replace("include/JSystem/J3D/J3DAnmTransform.h", "f32* mScaleVals;", "void* mNativeStorage; // owned host-order tables and values\n\tf32* mScaleVals;")
    replace("include/JSystem/J3D/J3DAnmTevRegKey.h", '#include "types.h"', '#include "types.h"\n#include "p2_game_alloc.h"')
    # J2D texture SRT animations own host-order tables like the other 2D keys.
    replace("include/JSystem/J2D/J2DAnm.h", "virtual ~J2DAnmTextureSRTKey() { }",
            "virtual ~J2DAnmTextureSRTKey() { p2_game_free(mNativeStorage); }\n\tvoid* mNativeStorage = nullptr; // owned host-order tables and values")
    replace("include/JSystem/J2D/J2DAnm.h", "virtual ~J2DAnmTevRegKey() { } // _08 (weak)",
            "virtual ~J2DAnmTevRegKey() { p2_game_free(mNativeStorage); } // _08 (weak)\n\tvoid* mNativeStorage = nullptr; // owned host-order tables and values")
    replace("include/JSystem/J3D/J3DAnmTevRegKey.h", ": mCRegNameTable()", ": mNativeStorage(nullptr)\n\t    , mCRegNameTable()")
    replace("include/JSystem/J3D/J3DAnmTevRegKey.h", "virtual ~J3DAnmTevRegKey() { }", "virtual ~J3DAnmTevRegKey() { p2_game_free(mNativeStorage); }")
    replace("include/JSystem/J3D/J3DAnmTevRegKey.h", "// _00-_0C = J3DAnmBase", "// _00-_0C = J3DAnmBase\n\tvoid* mNativeStorage;")
    replace("include/JSystem/J3D/J3DAnmTextureSRTKey.h", ": mUpdateMaterialName()", ": mNativeStorage(nullptr)\n\t    , mUpdateMaterialName()")
    replace("include/JSystem/J3D/J3DAnmTextureSRTKey.h", "virtual ~J3DAnmTextureSRTKey() { }", "virtual ~J3DAnmTextureSRTKey() { p2_game_free(mNativeStorage); }")
    replace("include/JSystem/J3D/J3DAnmTextureSRTKey.h", "// _00-_0C = J3DAnmBase", "// _00-_0C = J3DAnmBase\n\tvoid* mNativeStorage;")
    file = destination / "src/JSystem/J3D/J3DAnmLoader.cpp"
    text = '#include "p2_animation.h"\n' + file.read_text()
    for kind, key in (("Full", "false"), ("Key", "true")):
        pattern = rf"(void J3DAnm{kind}Loader_v15::setAnmTransform\([^\n]+\)\n)\{{.*?^\}}"
        text, count = re.subn(pattern, rf'\1{{\n\tif (!p2_load_transform(animation, data, data->mSize, {key}))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native transform animation\\n");\n}}', text, flags=re.M | re.S)
        if count != 1: raise RuntimeError("Transform loader patch drift")
    pattern = r"(void J3DAnmKeyLoader_v15::setAnmTevReg\([^\n]+\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_tev_animation(animation, data, data->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native TEV animation\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("TEV animation loader patch drift")
    pattern = r"(void J3DAnmKeyLoader_v15::setAnmColor\([^\n]+\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_color_key(animation, data, data->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native color animation\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("J3D color loader patch drift")
    # Remaining J3D loaders still cast disk offsets to 32-bit pointers and read
    # big-endian tables in place. Fail loudly until each has a native loader.
    for loader in ("J3DAnmFullLoader_v15::setAnmColor", "J3DAnmFullLoader_v15::setAnmTexPattern",
                   "J3DAnmFullLoader_v15::setAnmVisibility", "J3DAnmFullLoader_v15::setAnmCluster",
                   "J3DAnmFullLoader_v15::setAnmVtxColor", "J3DAnmKeyLoader_v15::setAnmCluster",
                   "J3DAnmKeyLoader_v15::setAnmVtxColor"):
        pattern = r"(void " + re.escape(loader) + r"\([^\n]+\)\n)\{.*?^\}"
        text, count = re.subn(pattern, r'\1{\n\tOSPanic(__FILE__, __LINE__, "J3D animation loader not ported to 64-bit: ' + loader + r'\\n");\n}', text, flags=re.M | re.S)
        if count != 1: raise RuntimeError("J3D loader panic patch drift: " + loader)
    replace("include/JSystem/J3D/J3DAnmColor.h", '#include "JSystem/J3D/J3DAnmBase.h"', '#include "JSystem/J3D/J3DAnmBase.h"\n#include "p2_game_alloc.h"')
    replace("include/JSystem/J3D/J3DAnmColor.h", "virtual ~J3DAnmColorKey() { }",
            "virtual ~J3DAnmColorKey() { p2_game_free(mNativeStorage); }\n\tvoid* mNativeStorage = nullptr; // owned host-order tables and values")
    pattern = r"(void J3DAnmKeyLoader_v15::setAnmTextureSRT\([^\n]+\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_texture_animation(animation, data, data->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native texture animation\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("Texture animation loader patch drift")
    file.write_text(text)
    replace("include/JSystem/J2D/J2DAnm.h", '#include "types.h"', '#include "types.h"\n#include "p2_game_alloc.h"')
    replace("include/JSystem/J2D/J2DAnm.h", "return fout;",
            "return JMAHermiteInterpolation(pp1, *pp2, *pp3, *pp4, *pp5, *pp6, *pp7);")
    replace("include/JSystem/J2D/J2DAnm.h", "mScaleVals       = pScaleValues;", "mNativeStorage = nullptr;\n\t\tmScaleVals       = pScaleValues;")
    replace("include/JSystem/J2D/J2DAnm.h", "virtual ~J2DAnmTransform() { }", "virtual ~J2DAnmTransform() { p2_game_free(mNativeStorage); }")
    replace("include/JSystem/J2D/J2DAnm.h", "f32* mScaleVals;       // _10", "void* mNativeStorage;\n\tf32* mScaleVals;       // _10")
    replace("include/JSystem/J2D/J2DAnm.h", "J2DAnmColorKey()\n\t{", "J2DAnmColorKey() : mNativeStorage(nullptr)\n\t{")
    replace("include/JSystem/J2D/J2DAnm.h", "virtual ~J2DAnmColorKey() { }", "virtual ~J2DAnmColorKey() { p2_game_free(mNativeStorage); }")
    replace("include/JSystem/J2D/J2DAnm.h", "// _00-_30 = J2DAnmColor\n\ts16* mRedVals;", "// _00-_30 = J2DAnmColor\n\tvoid* mNativeStorage;\n\ts16* mRedVals;")
    for member, occurrences in (("mMagic", 1), ("mType", 2), ("mCount", 1), ("mNextOffset", 1)):
        replace("include/JSystem/J2D/J2DAnmLoader.h", "u32 " + member + ";", "P2Big<u32> " + member + ";", occurrences)
    replace("include/JSystem/J2D/J2DAnm.h", "virtual ~J2DAnmVisibilityFull() { } // _08 (weak)", "virtual ~J2DAnmVisibilityFull() { p2_game_free(mNativeStorage); } // _08 (weak)")
    replace("include/JSystem/J2D/J2DAnm.h", "u8* mValues;                       // _18", "u8* mValues;                       // _18\n\tvoid* mNativeStorage = nullptr;")
    replace("include/JSystem/J2D/J2DAnm.h", "J2DAnmTexPattern()\n\t{", "J2DAnmTexPattern() : mNativeStorage(nullptr)\n\t{")
    replace("include/JSystem/J2D/J2DAnm.h", "virtual ~J2DAnmTexPattern() { delete[] mImgPtrArray; }", "virtual ~J2DAnmTexPattern() { delete[] mImgPtrArray; p2_game_free(mNativeStorage); }")
    replace("include/JSystem/J2D/J2DAnm.h", "u16* mValues;                              // _10", "void* mNativeStorage;\n\tu16* mValues;                              // _10")
    file = destination / "src/JSystem/J2D/J2DAnmLoader.cpp"
    text = '#include "p2_animation.h"\n#include "Dolphin/os.h"\n' + file.read_text()
    for kind, key in (("Full", "false"), ("Key", "true")):
        pattern = rf"(void J2DAnm{kind}Loader_v15::setAnmTransform\([^\n]+\)\n)\{{.*?^\}}"
        text, count = re.subn(pattern, rf'\1{{\n\tif (!p2_load_2d_transform(anm, data, data->mSize, {key}))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native 2D transform animation\\n");\n}}', text, flags=re.M | re.S)
        if count != 1: raise RuntimeError("2D transform loader patch drift")
    pattern = r"(void J2DAnmKeyLoader_v15::setAnmColor\([^\n]+\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_2d_color(anm, data, data->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native 2D color animation\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("2D color loader patch drift")
    pattern = r"(void J2DAnmKeyLoader_v15::setAnmTextureSRT\([^\n]+\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_2d_texture_srt(anm, data, data->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native 2D texture SRT animation\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("2D texture SRT loader patch drift")
    pattern = r"(void J2DAnmKeyLoader_v15::setAnmTevReg\([^\n]+\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_2d_tev_reg(anm, data, data->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native 2D TEV register animation\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("2D TEV register loader patch drift")
    # Remaining 2D loaders still cast disk offsets to 32-bit pointers and read
    # big-endian tables in place. Fail loudly until each has a native loader.
    # The disc uses none of these: a scan of every archive found only bck, bca, bpk, brk, btk,
    # btp and bva (J2D visibility, ported below), and no vertex-colour, cluster or full-colour data.
    for loader in ("J2DAnmKeyLoader_v15::setAnmVtxColor", "J2DAnmFullLoader_v15::setAnmColor",
                   "J2DAnmFullLoader_v15::setAnmVtxColor"):
        pattern = r"(void " + re.escape(loader) + r"\([^\n]+\)\n)\{.*?^\}"
        text, count = re.subn(pattern, r'\1{\n\tOSPanic(__FILE__, __LINE__, "2D animation loader not ported to 64-bit: ' + loader + r'\\n");\n}', text, flags=re.M | re.S)
        if count != 1: raise RuntimeError("2D loader panic patch drift: " + loader)
    pattern = r"(void J2DAnmFullLoader_v15::setAnmVisibility\([^\n]+\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_2d_visibility(anm, data, data->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native 2D visibility animation\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("2D visibility loader patch drift")
    pattern = r"(void J2DAnmFullLoader_v15::setAnmTexPattern\([^\n]+\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_2d_pattern(anm, data, data->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native 2D texture pattern animation\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("2D pattern loader patch drift")
    file.write_text(text)
    file = destination / "include/JSystem/JUtility/JUTNameTab.h"
    text = '#include "p2_endian.h"\n' + file.read_text()
    for member in ("mEntryNum", "mPad0", "mKeyCode", "mOffs"):
        text = text.replace("u16 " + member + ";", "P2Big<u16> " + member + ";")
    file.write_text(text)
    replace("src/JSystem/JUtility/JUTNameTab.cpp", "const ResNTAB::Entry* pEntry = mNameTable->mEntries;",
            "if (!mNameTable || !pName) return -1;\n\tconst ResNTAB::Entry* pEntry = mNameTable->mEntries;")
    # Every pointer-shaped member in J3DFileBlock is a 32-bit disk offset,
    # resolved by JSUConvertOffsetToPtr. None is a native runtime pointer.
    file = destination / "include/JSystem/J3D/J3DFileBlock.h"
    text = file.read_text()
    text = sub_exact(r"(?m)^(\s*)void\*(\s+m\w+(?:\[\d+\])?;)", r"\1P2Big<u32>\2", text, 78, "J3DFileBlock offsets")
    text = sub_exact(r"(?m)^(\s*)(u16|u32)(\s+m\w+;)", r"\1P2Big<\2>\3", text, 22, "J3DFileBlock scalars")
    file.write_text(text)
    replace("src/JSystem/J3D/J3DModelLoader.cpp", "((u32)nrm_end - (u32)vertData->mVtxNorm)",
            "((uintptr_t)nrm_end - (uintptr_t)vertData->mVtxNorm)")
    replace("src/JSystem/J3D/J3DModelLoader.cpp", "((u32)color0_end - (u32)vertData->mVtxColor[0])",
            "((uintptr_t)color0_end - (uintptr_t)vertData->mVtxColor[0])")
    for name in ("J3DModelLoader", "J3DModelLoaderCalcSize", "J3DMaterialFactory"):
        file = destination / f"src/JSystem/J3D/{name}.cpp"
        text = re.sub(r"(block(?:->|\.)m\w+Offset) != nullptr", r"\1 != 0", file.read_text())
        text = text.replace("(const void*)block->mNameTableOffset", "u32(block->mNameTableOffset)")
        # Material identity occupies a 32-bit flag word. Preserve console cached
        # address bits via an explicit arena offset, never truncate host pointers.
        for pointer in ("&mMaterialTable->mUniqueMaterials[i]", "&mMaterialTable->mUniqueMaterials[factory.getMaterialID(i)]",
                        "(mMaterialTable->mMaterials)", "mMaterialTable->mMaterials"):
            text = text.replace("(u32)" + pointer, "(0x80000000u | p2_host_to_physical(" + pointer + "))")
        file.write_text(text)
    replace("include/Dolphin/GX/GXFifo.h", "#define GXWGFifo (*(volatile PPCWGPipe*)GXFIFO_ADDR)",
            '#include "p2_gx_fifo.h"\n#define GXWGFifo p2_gx_fifo')
    replace("include/Dolphin/GX/GXFifo.h", "static inline void GXEnd(void)\n{\n}", "void GXEnd(void);")
    replace("include/Dolphin/GX/GXTypes.h", "u8 pad[0x20]; // _00", "u32 pad[16]; // native Aurora GXTexObj", count=1)
    replace("include/Dolphin/GX/GXTypes.h", "u8 padding[0xc]; // _00", "u32 padding[10]; // native Aurora GXTlutObj", count=1)
    replace("src/JSystem/J3D/J3DShape.cpp", "GDSetArrayRaw((GXAttr)(i + GX_VA_POS), nullptr, stride[i]);",
            "GDSetArrayRaw((GXAttr)(i + GX_VA_POS), 0, stride[i]);")
    replace("src/JSystem/J3D/J3DShape.cpp", "((u32)data & 0x7FFFFFFF)", "(data ? p2_host_to_physical(data) : 0)")
    file = destination / "src/JSystem/J3D/J3DGD.cpp"
    file.write_text(file.read_text().replace("GX2HWFiltConv", "J3DGX2HWFiltConv"))
    file = destination / "src/JSystem/JKernel/JKRMemArchive.cpp"
    text = file.read_text()
    for function_pattern in (r"bool JKRMemArchive::open\(s32 entryNum, EMountDirection mountDirection\)",
                      r"bool JKRMemArchive::open\(void\* buffer, u32 bufferSize, JKRMemBreakFlag flag\)",
                      r"u32 JKRMemArchive::fetchResource_subroutine\([^\n]+\)"):
        text, count = re.subn(function_pattern + r"\n\{.*?^\}", "// Implemented in native src/archive.cpp.", text, flags=re.M | re.S)
        if count != 1: raise RuntimeError("Native archive implementation patch drift")
    text = text.replace("if (mIsMounted == true) {", "if (mIsMounted == true) {\n\t\tJKRHeap::free(mDataInfo, nullptr);")
    text = text.replace("JKRFreeToHeap(mHeap, mHeader);", "JKRHeap::free(mHeader, nullptr);")
    # Compressed input length is independent of the destination capacity.
    text = text.replace("fetchResource_subroutine((u8*)data, srcLength,", "fetchResource_subroutine((u8*)data, entry->mSize,")
    file.write_text(text)
    file = destination / "src/JSystem/JKernel/JKRArchivePub.cpp"
    text = '#include "p2_endian.h"\n' + file.read_text()
    text = text.replace("JKRMemArchive(mem, 0xFFFF, MBF_0)", "JKRMemArchive(mem, p2_big_endian(static_cast<u32*>(mem)[1]), MBF_0)")
    text = text.replace("\treturn new (heap, (mountDirection == EMD_Head) ? 4 : -4) JKRMemArchive(mem, p2_big_endian(static_cast<u32*>(mem)[1]), MBF_0);",
                        "\tif (!mem) return nullptr;\n\tarchive = new (heap, (mountDirection == EMD_Head) ? 4 : -4) JKRMemArchive(mem, p2_big_endian(static_cast<u32*>(mem)[1]), MBF_0);\n\tif (archive && archive->getMountMode() == EMM_Unk0) { delete archive; return nullptr; }\n\treturn archive;")
    # Native hosts keep all archive payloads in main memory, including archives
    # which the console split across ARAM/DVD. Resource API semantics stay intact.
    for name in ("JKRAramArchive", "JKRDvdArchive", "JKRCompArchive"):
        text = text.replace(f"new (heap, i) {name}(", "new (heap, i) JKRMemArchive(")
    text = text.replace("JKRArchive* archive;", "JKRArchive* archive = nullptr;")
    file.write_text(text)
    # Keep the original file-opening wrappers; use the bounded native reader
    # instead of the console's global decoder buffers and retrace retry loop.
    file = destination / "src/JSystem/JKernel/JKRDvdRipper.cpp"
    wrappers = re.findall(r"void\* JKRDvdRipper::loadToMainRAM\((?:const char\*|s32)[\s\S]*?^\}", file.read_text(), flags=re.M)
    if len(wrappers) != 2: raise RuntimeError("DVD ripper wrapper patch drift")
    file.write_text('#include "JSystem/JKernel/JKRDvdRipper.h"\n' + "\n\n".join(wrappers) + "\n")
    # Heap diagnostics must work before the game's graphical console exists.
    replace("include/ARAM.h", "inline Node();", "Node();")
    replace("include/ARAM.h", "inline u32 dvdToAram", "u32 dvdToAram")
    replace("include/ARAM.h", "virtual ~Node() { }", "virtual ~Node();")
    replace("include/ARAM.h", "JKRAramBlock* mMemoryBlock;", "bool mNativeOwnName = false;\n\tJKRAramBlock* mMemoryBlock;")
    replace("include/ARAM.h", "\tMgr();", "\tMgr();\n\t~Mgr();\n\tOSMutex mNativeMutex;")
    file = destination / "src/sysGCU/pikmin2AramMgr.cpp"
    text, count = re.subn(r"void Mgr::loadEnemy\(\)\n\{.*?^\}", "// Native bounded list reader: src/resource_cache.cpp.", file.read_text(), flags=re.M | re.S)
    if count != 1: raise RuntimeError("Enemy resource loader patch drift")
    file.write_text(text.replace("\t// UNUSED FUNCTION\n}", "\treturn !mLoadPermission;\n}"))
    replace("src/sysCommonU/node.cpp", '#include "Dolphin/os.h"', '#include "Dolphin/os.h"\n#undef JUT_ASSERTLINE\n#define JUT_ASSERTLINE(line, cond, ...) do { if (!(cond)) OSPanic(__FILE__, line, __VA_ARGS__); } while (0)')
    # ARAM addresses remain bounded 32-bit offsets; callback payloads are native
    # pointers. Main-memory transfer addresses explicitly use MEM1 offsets.
    replace("include/Dolphin/ar.h", "(*ARQCallback)(u32 ptrToRequest)", "(*ARQCallback)(uintptr_t ptrToRequest)")
    replace("include/JSystem/JKernel/JKRAram.h", "static void doneDMA(u32 cmdAddr);", "static void doneDMA(uintptr_t cmdAddr);")
    replace("include/JSystem/JKernel/JKRDvdAramRipper.h", "(*LoadCallback)(u32)", "(*LoadCallback)(uintptr_t)")
    file = destination / "src/JSystem/JKernel/JKRDvdAramRipper.cpp"
    wrappers = re.findall(r"JKRAramBlock\* JKRDvdAramRipper::loadToAram\((?:const char\*|s32)[\s\S]*?^\}", file.read_text(), flags=re.M)
    if len(wrappers) != 2: raise RuntimeError("DVD-to-ARAM wrapper patch drift")
    file.write_text('#include "JSystem/JKernel/JKRDvdAramRipper.h"\n' + "\n\n".join(wrappers).replace("\tJKRDvdFile file;", "\tif (sizePtr) *sizePtr = 0;\n\tJKRDvdFile file;") + "\n")
    # The game's resource manager only tests this result for success; it never
    # consumes the old truncated pointer as an address or handle.
    replace("src/sysGCU/aramMgr.cpp", "return reinterpret_cast<u32>(mMemoryBlock);", "return mMemoryBlock != nullptr;")
    (destination / "src/sysGCU/aramMgr.cpp").write_text('// Native resource cache: src/resource_cache.cpp.\n#include "ARAM.h"\n')
    replace("include/JSystem/JKernel/JKRAram.h", "struct JKRAramBlock {", "struct JKRAramBlock {\n\tJKRAramHeap* mNativeHeap = nullptr;")
    (destination / "src/JSystem/JKernel/JKRAram.cpp").write_text('// Native implementation: src/aram.cpp.\n#include "JSystem/JKernel/JKRAram.h"\n')
    file = destination / "src/JSystem/JKernel/JKRAramPiece.cpp"
    original = file.read_text()
    kept = re.findall(r"JKRAMCommand::(?:JKRAMCommand|~JKRAMCommand)\(\)[\s\S]*?^\}", original, flags=re.M)
    if len(kept) != 2: raise RuntimeError("ARAM command patch drift")
    file.write_text('#include "JSystem/JKernel/JKRAram.h"\n' + "\n\n".join(kept) + "\n")
    replace("src/JSystem/JKernel/JKRAramPiece.cpp", "JKRAMCommand::~JKRAMCommand()\n{", "JKRAMCommand::~JKRAMCommand()\n{\n\tp2_destroy_message_queue(&mMessageQueue);")
    replace("src/JSystem/JKernel/JKRAramHeap.cpp", "\tsAramList.append(&block->mLink);", '\tif (!block) OSPanic(__FILE__, __LINE__, "ARAM sentinel allocation failed");\n\tblock->mNativeHeap = this;\n\tsAramList.append(&block->mLink);', count=2)
    replace("src/JSystem/JKernel/JKRAramHeap.cpp", "\t\tdelete (iterator++).getObject();\n\t}\n}", "\t\tdelete (iterator++).getObject();\n\t}\n\tp2_destroy_mutex(&mMutex);\n}")
    for name in ("allocFromHead", "allocFromTail"):
        signature_text = f"JKRAramBlock* JKRAramHeap::{name}(u32 size)\n{{"
        replace("src/JSystem/JKernel/JKRAramHeap.cpp", signature_text, signature_text + "\n\tif (!size || size > mSize) return nullptr;")
    file = destination / "src/JSystem/JKernel/JKRAramBlock.cpp"
    text = file.read_text()
    text, count = re.subn(r"(JKRAramBlock\* block = new [^\n]+;)", r"\1\n\tif (!block) return nullptr;\n\tblock->mNativeHeap = heap;", text)
    if count != 2: raise RuntimeError("ARAM block allocation patch drift")
    text = text.replace("JKRAramBlock::~JKRAramBlock()\n{", "JKRAramBlock::~JKRAramBlock()\n{\n\tif (mNativeHeap) mNativeHeap->lock();")
    text = text.replace("\t\tmSize = 0;\n\t}\n}", "\t\tmSize = 0;\n\t}\n\tif (list) list->remove(&mLink);\n\tif (mNativeHeap) mNativeHeap->unlock();\n}")
    file.write_text(text)
    # Console thread switches multiplexed one current-heap global. Native
    # workers run concurrently, so each inherits and changes its own selection.
    replace("include/JSystem/JKernel/JKRHeap.h", "static JKRHeap* sCurrentHeap;", "static thread_local JKRHeap* sCurrentHeap;")
    replace("src/JSystem/JKernel/JKRHeap.cpp", "JKRHeap* JKRHeap::sCurrentHeap;", "thread_local JKRHeap* JKRHeap::sCurrentHeap;")
    replace("src/JSystem/JKernel/JKRThread.cpp", "mCurrentHeap      = 0;", "mCurrentHeap      = JKRHeap::sCurrentHeap;")
    replace("src/JSystem/JKernel/JKRThread.cpp", "return static_cast<JKRThread*>(thread)->run();",
            "auto* self = static_cast<JKRThread*>(thread);\n\tJKRHeap::sCurrentHeap = self->mCurrentHeap;\n\treturn self->run();")
    replace("include/DvdThreadCommand.h", '#include "types.h"', '#include "types.h"\n#include "p2_memory.h"\n#include <atomic>')
    replace("include/DvdThreadCommand.h", "~DvdThreadCommand() {};", "~DvdThreadCommand();")
    replace("include/DvdThreadCommand.h", "int mMode;", "std::atomic<int> mMode;")
    replace("include/DvdThreadCommand.h", "OSMutex mMutex;", "bool mNativeQueued = false;\n\tJKRHeap* mNativeSubmissionHeap = nullptr;\n\tJKRHeap** mNativeSubmitterHeap = nullptr;\n\tOSMutex mMutex;")
    replace("include/DvdThreadCommand.h", "virtual ~DvdThread() { }", "virtual ~DvdThread();")
    replace("include/DvdThreadCommand.h", "JSUList<DvdThreadCommand> mCommandList;", "OSMutex mNativeListMutex;\n\tJSUList<DvdThreadCommand> mCommandList;")
    file = destination / "src/sysGCU/dvdThread.cpp"
    original = file.read_text()
    kept = []
    for pattern in (r"DvdThreadCommand::DvdThreadCommand\(\)", r"void DvdThreadCommand::loadUseCallBack\([^\n]*\)",
                    r"void DvdThreadCommand::invokeCallBack\(\)", r"DvdThread::DvdThread\([^\n]*\)",
                    r"void DvdThread::loadArchive\([^\n]*\)"):
        matches = re.findall(pattern + r"[\s\S]*?^\}", original, flags=re.M)
        if len(matches) != 1: raise RuntimeError("DVD worker patch drift: " + pattern)
        kept += matches
    file.write_text(original[:original.index('/**')] + "\n\n".join(kept) + "\n")
    replace("src/sysGCU/dvdThread.cpp", "\tOSResumeThread(mThread);", "\tOSInitMutex(&mNativeListMutex);\n\tOSResumeThread(mThread);")
    replace("src/sysGCU/dvdThread.cpp", "P2ASSERTLINE(132, mCallBack);", 'if (!mCallBack) OSPanic(__FILE__, __LINE__, "Missing DVD callback");')
    replace("src/sysGCU/dvdThread.cpp", "P2ASSERTLINE(275, arc);", 'if (!arc) OSPanic(__FILE__, __LINE__, "Cannot mount DVD archive");')
    replace("src/JSystem/JKernel/JKRThread.cpp", "(u32)thread->stackEnd - (u32)thread->stackBase",
            "static_cast<u32>(reinterpret_cast<uintptr_t>(thread->stackBase) - reinterpret_cast<uintptr_t>(thread->stackEnd))")
    replace("src/JSystem/JKernel/JKRThread.cpp", "mMsgCount << 2", "mMsgCount * sizeof(OSMessage)")
    replace("src/JSystem/JKernel/JKRThread.cpp", "(void*)((u32)mStack + mStackSize)", "static_cast<u8*>(mStack) + mStackSize")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\t\tJKRHeap::free(mStack, mHeap);", "\t\tp2_destroy_thread(mThread);\n\t\tJKRHeap::free(mStack, mHeap);")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\tJKRHeap::free(mMsgBuffer, nullptr);", "\tp2_destroy_message_queue(&mMsgQueue);\n\tJKRHeap::free(mMsgBuffer, nullptr);")
    # Task workers must not start before create() has populated their messages.
    replace("include/JSystem/JKernel/JKRThread.h", "struct JKRTask : public JKRThread {", "struct JKRTask : public JKRThread {\n\tOSMutex mNativeRequestMutex;")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\tOSResumeThread(mThread);", "\t_8C = nullptr; _90 = 0;\n\tOSInitMutex(&mNativeRequestMutex);\n\t// Started by create after message storage is initialized.")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\tsTaskList.append(&task->_7C);", "\tsTaskList.append(&task->_7C);\n\ttask->resume();")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\tsTaskList.remove(&_7C);", "\tp2_destroy_thread(mThread);\n\tdelete[] _8C;\n\tp2_destroy_mutex(&mNativeRequestMutex);\n\tsTaskList.remove(&_7C);")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\tMessage* msg = searchBlank();", "\tOSLockMutex(&mNativeRequestMutex);\n\tMessage* msg = searchBlank();")
    replace("src/JSystem/JKernel/JKRThread.cpp", "if (msg == nullptr) {\n\t\treturn false;", "if (msg == nullptr) {\n\t\tOSUnlockMutex(&mNativeRequestMutex);\n\t\treturn false;")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\treturn sendResult;", "\tOSUnlockMutex(&mNativeRequestMutex);\n\treturn sendResult;")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\t\tmsg->_00 = nullptr;\n\t}\n}", "\t\tOSLockMutex(&mNativeRequestMutex);\n\t\tmsg->_00 = nullptr;\n\t\tOSUnlockMutex(&mNativeRequestMutex);\n\t}\n}")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\t\tmsg = (Message*)waitMessageBlock();", "\t\tmsg = (Message*)waitMessageBlock();\n\t\tconst int cancellationState = p2_begin_thread_work();")
    replace("src/JSystem/JKernel/JKRThread.cpp", "\t\tOSUnlockMutex(&mNativeRequestMutex);\n\t}\n}", "\t\tOSUnlockMutex(&mNativeRequestMutex);\n\t\tp2_end_thread_work(cancellationState);\n\t}\n}")
    replace("include/JSystem/JKernel/JKRFile.h", "DVDFileInfo mDvdPlayer;", "JKRDVDFileInfo mDvdPlayer;")
    replace("src/JSystem/JKernel/JKRDvdFile.cpp", "mDvdFile = this;", "mDvdFile = this;\n\tmDvdPlayer.mFile = this;")
    replace("src/JSystem/JKernel/JKRDvdFile.cpp", "return (u32)*buffer;", "return static_cast<s32>(reinterpret_cast<intptr_t>(*buffer));")
    replace("src/JSystem/JKernel/JKRDvdFile.cpp", "(void*)result", "reinterpret_cast<void*>(static_cast<intptr_t>(result))")
    replace("src/JSystem/JKernel/JKRDvdFile.cpp", "JKRDvdFile::~JKRDvdFile()\n{\n\tclose();",
            "JKRDvdFile::~JKRDvdFile()\n{\n\tclose();\n\tp2_destroy_message_queue(&mMessageQueue1);\n\tp2_destroy_message_queue(&mMessageQueue2);\n\tp2_destroy_mutex(&mDvdMutex);\n\tp2_destroy_mutex(&mAramMutex);")
    replace("src/JSystem/JSupport/JSUFileStream.cpp", "int readBytes = 0;", "if (!mObject || byteCount <= 0) return 0;\n\tint readBytes = 0;")
    replace("src/JSystem/JSupport/JSUFileStream.cpp", "(u32)(mLength + byteCount) > ((JKRFile*)mObject)->getFileSize()",
            "byteCount > ((JKRFile*)mObject)->getFileSize() - mLength")
    replace("src/JSystem/JSupport/JSUFileStream.cpp", "u32 originalLength = mLength;",
            "if (!mObject) return 0;\n\ts32 originalLength = mLength;\n\tint64_t position = mLength;")
    replace("src/JSystem/JSupport/JSUFileStream.cpp", "mLength = offset;", "position = offset;")
    replace("src/JSystem/JSupport/JSUFileStream.cpp", "mLength = ((JKRFile*)mObject)->getFileSize() - offset;", "position = int64_t(((JKRFile*)mObject)->getFileSize()) - offset;")
    replace("src/JSystem/JSupport/JSUFileStream.cpp", "mLength += offset;", "position += offset;")
    replace("src/JSystem/JSupport/JSUFileStream.cpp", "if (0 > mLength)",
            "const s32 length = ((JKRFile*)mObject)->getFileSize();\n\tmLength = position < 0 ? 0 : position > length ? length : s32(position);\n\tif (0 > mLength)")
    replace("include/JSystem/J3D/J3DJointTree.h", '#include "types.h"', '#include "types.h"\n#include "p2_game_alloc.h"\n#include "p2_skin.h"')
    replace("include/JSystem/J3D/J3DJointTree.h", "J3DJointTree();", "J3DJointTree();\n\tP2EnvelopeData* mNativeEnvelope = nullptr;")
    replace("include/JSystem/J3D/J3DJointTree.h", "virtual ~J3DJointTree() {};", "virtual ~J3DJointTree() { p2_game_free(mNativeEnvelope); };")
    file = destination / "src/JSystem/J3D/J3DModelLoader.cpp"
    text = '#include "p2_vertex.h"\n' + file.read_text()
    pattern = r"(void J3DModelLoader::readVertex\(const J3DVertexBlock\* block\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tif (!p2_load_vertex(mModelData->getVertexData(), block, block->mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native vertex data\\n");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("Vertex loader patch drift")
    pattern = r"(void J3DModelLoader::readEnvelop\(const J3DEnvelopeBlock\* block\)\n)\{.*?^\}"
    text, count = re.subn(pattern, r'\1{\n\tauto& tree=mModelData->getJointTree();\n\tauto* native=p2_load_envelope(block, block->mSize);\n\tif (!native) OSPanic(__FILE__, __LINE__, "Invalid native envelope data\\n");\n\tp2_game_free(tree.mNativeEnvelope); tree.mNativeEnvelope=native;\n\ttree.mEnvelopeCnt=native->count; tree.mEnvelopeMixCnt=native->counts;\n\ttree.mEnvelopeMixIdx=native->indices; tree.mEnvelopeMixWeight=native->weights;\n\ttree.mInvJointMtx=native->inverseBind;\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("Envelope loader patch drift")
    file.write_text(text)
    replace("include/JSystem/J3D/J3DModel.h", "struct J3DModelHierarchy {\n\tu16 mType;  // _00\n\tu16 mValue; // _02",
            "struct J3DModelHierarchy {\n\tP2Big<u16> mType;  // disk hierarchy opcode\n\tP2Big<u16> mValue; // disk joint/material/shape index")
    replace("include/JSystem/J3D/J3DDrawMtxData.h", '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"')
    replace("include/JSystem/J3D/J3DDrawMtxData.h", "u16* mDrawMtxIdx;", "P2Big<u16>* mDrawMtxIdx;")
    replace("src/JSystem/J3D/J3DModelLoader.cpp", "JSUConvertOffsetToPtr<u16>(block, block->mDataArrayOffset)",
            "JSUConvertOffsetToPtr<P2Big<u16>>(block, block->mDataArrayOffset)")
    replace("src/JSystem/J3D/J3DShapeFactory.cpp", "mVcdVatCmdBuffer + id * 0xC0",
            "mVcdVatCmdBuffer + id * J3DShape::kVcdVatDLSize")
    file = destination / "include/JSystem/J2D/J2DMaterialFactory.h"
    text = '#include "p2_endian.h"\n#include "p2_game_alloc.h"\n' + file.read_text()
    start = text.index("struct J2DMaterialBlock {")
    end = text.index("\n};", start)
    fields = sub_exact(r"\bu32 (\w+);", r"P2Big<u32> \1;", text[start:end], 23, "J2DMaterialBlock")
    fields = fields.replace("u16 _08;", "P2Big<u16> _08;")
    text = text[:start] + fields + text[end:]
    text = text.replace("J2DMaterialFactory(J2DMaterialBlock const&);", "J2DMaterialFactory(J2DMaterialBlock const&);\n\t~J2DMaterialFactory() { p2_game_free(mNativeStorage); }\n\tvoid* mNativeStorage = nullptr;")
    file.write_text(text)
    file = destination / "src/JSystem/J2D/J2DMaterialFactory.cpp"
    text = '#include "p2_material.h"\n#include "Dolphin/os.h"\n' + file.read_text()
    old = "J2DMaterialFactory::J2DMaterialFactory(const J2DMaterialBlock& header)\n{"
    if text.count(old) != 1: raise RuntimeError("2D material factory patch drift")
    text = text.replace("(void*)header.", "header.")
    text = text.replace(old, 'J2DMaterialFactory::J2DMaterialFactory(const J2DMaterialBlock& disk)\n{\n\tmNativeStorage = p2_decode_2d_material(&disk, p2_read_big<u32>(reinterpret_cast<const u8*>(&disk)+4));\n\tif (!mNativeStorage) OSPanic(__FILE__, __LINE__, "Invalid native MAT1 data");\n\tconst J2DMaterialBlock& header = *static_cast<const J2DMaterialBlock*>(mNativeStorage);')
    file.write_text(text)
    replace("include/JSystem/J3D/J3DShapeFactory.h", '#include "types.h"', '#include "types.h"\n#include "p2_game_alloc.h"')
    replace("include/JSystem/J3D/J3DMaterialFactory.h", '#include "types.h"', '#include "types.h"\n#include "p2_game_alloc.h"')
    replace("include/JSystem/J3D/J3DMaterialFactory.h", "J3DMaterialFactory(const J3DMaterialBlock& block);",
            "J3DMaterialFactory(const J3DMaterialBlock& block);\n\t~J3DMaterialFactory() { p2_game_free(mNativeStorage); }\n\tvoid* mNativeStorage = nullptr;")
    file = destination / "src/JSystem/J3D/J3DMaterialFactory.cpp"
    text = '#include "p2_material.h"\n' + file.read_text()
    old = "J3DMaterialFactory::J3DMaterialFactory(const J3DMaterialBlock& block)\n{"
    if text.count(old) != 1: raise RuntimeError("Material factory patch drift")
    text = text.replace(old, 'J3DMaterialFactory::J3DMaterialFactory(const J3DMaterialBlock& disk)\n{\n\tmNativeStorage = p2_decode_material(&disk, disk.mSize);\n\tif (!mNativeStorage) OSPanic(__FILE__, __LINE__, "Invalid native MAT3 data");\n\tconst J3DMaterialBlock& block = *static_cast<const J3DMaterialBlock*>(mNativeStorage);')
    file.write_text(text)
    replace("include/JSystem/J3D/J3DShapeFactory.h", "J3DShapeFactory(const J3DShapeBlock&);",
            "J3DShapeFactory(const J3DShapeBlock&);\n\t~J3DShapeFactory() { p2_game_free(mNativeStorage); }\n\tvoid* mNativeStorage = nullptr;")
    file = destination / "src/JSystem/J3D/J3DShapeFactory.cpp"
    text = '#include "p2_shape.h"\n' + file.read_text()
    text, count = re.subn(r"J3DShapeFactory::J3DShapeFactory\(const J3DShapeBlock& block\).*?^\}",
                         'J3DShapeFactory::J3DShapeFactory(const J3DShapeBlock& block)\n{\n\tif (!p2_load_shape(this, &block, block.mSize))\n\t\tOSPanic(__FILE__, __LINE__, "Invalid native shape data");\n}', text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("Shape factory patch drift")
    file.write_text(text)
    replace("include/JSystem/J3D/J3DShape.h", '#include "types.h"', '#include "types.h"\n#include "p2_game_alloc.h"')
    replace("include/JSystem/J3D/J3DShape.h", "virtual ~J3DShapeTable() { }",
            "void* mNativeStorage = nullptr;\n\tvirtual ~J3DShapeTable() { p2_game_free(mNativeStorage); }")
    replace("src/JSystem/J3D/J3DModelLoader.cpp", "J3DShapeFactory factory(*block);",
            "J3DShapeFactory factory(*block);\n\tp2_game_free(model->mShapeTable.mNativeStorage);\n\tmodel->mShapeTable.mNativeStorage = factory.mNativeStorage;\n\tmShapeBlock = static_cast<const J3DShapeBlock*>(factory.mNativeStorage); // setupBBoardInfo reads decoded remaps\n\tfactory.mNativeStorage = nullptr;")
    replace("include/Dolphin/gd.h", "void GDSetArrayRaw(GXAttr attr, u32 data, u8 stride);",
            "void GDSetArrayRaw(GXAttr attr, u32 data, u8 stride);\nvoid GDSetArraySized(GXAttr attr, void* data, u32 size, u8 stride, bool littleEndian);")
    # Aurora's GXSetArray ABI adds an explicit byte extent and byte order.
    # Match every game caller; a console three-argument call cannot link safely.
    replace("include/Dolphin/GX/GXGeometry.h", "extern void GXSetArray(GXAttr attr, void* basePtr, u8 stride);",
            "extern void GXSetArray(GXAttr attr, const void* basePtr, u32 size, u8 stride, bool littleEndian);")
    replace("src/plugProjectKandoU/texCaster.cpp", "GXSetArray(GX_VA_POS, mVertices, sizeof(Vector3f));",
            "GXSetArray(GX_VA_POS, mVertices, mTriangleCount * 3 * sizeof(Vector3f), sizeof(Vector3f), true);")
    replace("src/plugProjectKandoU/texCaster.cpp", "GXSetArray(GX_VA_TEX0, mTexturePositions, 8);",
            "GXSetArray(GX_VA_TEX0, mTexturePositions, mTriangleCount * 6 * sizeof(f32), 8, true);")
    replace("src/plugProjectNishimuraU/ShadowCylinder.cpp", "GXSetArray(GX_VA_POS, sCylinderVertPos, 0xc);",
            "GXSetArray(GX_VA_POS, sCylinderVertPos, sizeof(sCylinderVertPos), 0xc, true);", count=2)
    replace("src/JSystem/JParticle/JPAResource.cpp", "GXSetArray(GX_VA_POS, jpa_pos + pos_offset, 3);",
            "GXSetArray(GX_VA_POS, jpa_pos + pos_offset, sizeof(jpa_pos) - pos_offset, 3, false);", count=2)
    replace("src/JSystem/JParticle/JPAResource.cpp", "GXSetArray(GX_VA_TEX0, jpa_crd + crd_offset, 2);",
            "GXSetArray(GX_VA_TEX0, jpa_crd + crd_offset, sizeof(jpa_crd) - crd_offset, 2, false);")
    replace("src/JSystem/JParticle/JPAResource.cpp", "GXSetArray(GX_VA_TEX0, jpa_crd, 2);",
            "GXSetArray(GX_VA_TEX0, jpa_crd, sizeof(jpa_crd), 2, false);")
    replace("include/JSystem/J3D/J3DSys.h", "void setModelDrawMtx(Mtx* pMtxArr)", "void setModelDrawMtx(Mtx* pMtxArr, u32 count)")
    replace("include/JSystem/J3D/J3DSys.h", "void setModelNrmMtx(Mtx* pMtxArr)", "void setModelNrmMtx(Mtx* pMtxArr, u32 count)")
    replace("include/JSystem/J3D/J3DSys.h", "GXSetArray(GX_POS_MTX_ARRAY, mModelDrawMtx, sizeof(*mModelDrawMtx));",
            "GXSetArray(GX_POS_MTX_ARRAY, mModelDrawMtx, count * sizeof(Mtx), sizeof(Mtx), true);")
    replace("include/JSystem/J3D/J3DSys.h", "GXSetArray(GX_NRM_MTX_ARRAY, mModelNormMtx, sizeof(Mtx33));",
            "GXSetArray(GX_NRM_MTX_ARRAY, mModelNormMtx, count * sizeof(Mtx33), sizeof(Mtx33), true);")
    replace("src/JSystem/J3D/J3DShape.cpp", "j3dSys.setModelDrawMtx(mDrawMtx[*mCurrentViewNumber]);",
            "j3dSys.setModelDrawMtx(mDrawMtx[*mCurrentViewNumber], mDrawMtxData->mCount);")
    replace("src/JSystem/J3D/J3DShape.cpp", '#include "JSystem/J3D/J3DShape.h"',
            '#include "JSystem/J3D/J3DShape.h"\n#include "JSystem/J3D/J3DDrawMtxData.h"')
    replace("src/JSystem/J3D/J3DShape.cpp", "j3dSys.setModelNrmMtx((Mtx*)mNrmMtx[*mCurrentViewNumber]);",
            "j3dSys.setModelNrmMtx((Mtx*)mNrmMtx[*mCurrentViewNumber], mDrawMtxData->mCount);")
    matrix_count = "(j3dSys.mModel->mModelData->mJointTree.mMtxData.mDrawMtxFlag[mUseMtxIndex] ? j3dSys.mModel->mModelData->mJointTree.getWEvlpMtxNum() : j3dSys.mModel->mModelData->getJointNum())"
    replace("src/JSystem/J3D/J3DShapeMtx.cpp", "GXSetArray(GX_POS_MTX_ARRAY, j3dSys.mModelDrawMtx, sizeof(Mtx));",
            f"GXSetArray(GX_POS_MTX_ARRAY, j3dSys.mModelDrawMtx, {matrix_count} * sizeof(Mtx), sizeof(Mtx), true);")
    replace("src/JSystem/J3D/J3DShapeMtx.cpp", "j3dSys.setModelDrawMtx((Mtx*)sMtxPtrTbl[draw_mtx_flag]);",
            "j3dSys.setModelDrawMtx((Mtx*)sMtxPtrTbl[draw_mtx_flag], draw_mtx_flag ? j3dSys.mModel->mModelData->mJointTree.getWEvlpMtxNum() : j3dSys.mModel->mModelData->getJointNum());")
    replace("src/JSystem/J3D/J3DShapeMtx.cpp", "j3dSys.setModelDrawMtx((Mtx*)sMtxPtrTbl[0]);",
            "j3dSys.setModelDrawMtx((Mtx*)sMtxPtrTbl[0], j3dSys.mModel->mModelData->getJointNum());")
    replace("include/JSystem/J3D/J3DShape.h", "kVcdVatDLSize = 0xC0,", "kVcdVatDLSize = 0x180, // room for Aurora's sized 64-bit array commands")
    replace("src/JSystem/J3D/J3DShape.cpp",
            "if (array[i] != 0)\n\t\t\tGDSetArray((GXAttr)(i + GX_VA_POS), array[i], stride[i]);\n\t\telse\n\t\t\tGDSetArrayRaw((GXAttr)(i + GX_VA_POS), 0, stride[i]);",
            "const u32 slot = (i == 1 && mHasNBT) ? 12 : i;\n\t\tGDSetArraySized((GXAttr)(i + GX_VA_POS), array[i], array[i] ? mVtxData->mNativeArrayBytes[slot] : 0, stride[i], mVtxData->mNativeArrayLittleEndian[slot]);")
    replace("src/JSystem/J3D/J3DShape.cpp", "static void J3DLoadArrayBasePtr(_GXAttr attr, void* data)",
            "static void J3DLoadArrayBasePtr(_GXAttr attr, void* data, u32 size, bool littleEndian)")
    replace("src/JSystem/J3D/J3DShape.cpp", "J3DLoadCPCmd(0xA0 + idx, (data ? p2_host_to_physical(data) : 0));",
            "p2_gx_array_base(attr, data, data ? size : 0, littleEndian);")
    for attr, method, slot in (("POS", "getVtxPos", 0), ("NRM", "getVtxNrm", 1), ("CLR0", "getVtxCol", 2)):
        replace("src/JSystem/J3D/J3DShape.cpp", f"J3DLoadArrayBasePtr(GX_VA_{attr}, j3dSys.{method}());",
                f"J3DLoadArrayBasePtr(GX_VA_{attr}, j3dSys.{method}(), mVtxData->mNativeArrayBytes[{slot}], mVtxData->mNativeArrayLittleEndian[{slot}]);", count=3)
    replace("src/JSystem/JKernel/JKRHeap.cpp", "\tdo_resize(memoryBlock, newSize);", "\treturn do_resize(memoryBlock, newSize);")
    for name in ("JKRHeap", "JKRExpHeap", "JKRSolidHeap"):
        file = destination / f"src/JSystem/JKernel/{name}.cpp"
        text = '#include "p2_memory.h"\n' + file.read_text()
        for kind in ("Report", "Warning"):
            text = text.replace(f"JUT{kind}Console_f(", "p2_heap_reportf(")
            text = text.replace(f"JUT{kind}Console(", "p2_heap_report(")
        file.write_text(text)
    # Native actor layouts cannot use the console's encoded byte offsets
    # (301 - 1 == 0x12c; 305 - 1 == 0x130) to select animation modes.
    replace("include/JSystem/JStudio_JStage.h",
            "TVVOutput_ANIMATION_FRAME_(int valueIndex, u32 val, Setter setter, Getter getter, MaxGetter maxGetter)",
            "TVVOutput_ANIMATION_FRAME_(int valueIndex, u32 TAdaptor_actor::*mode, Setter setter, Getter getter, MaxGetter maxGetter)")
    replace("include/JSystem/JStudio_JStage.h", ", _08(val)", ", mMode(mode)")
    replace("include/JSystem/JStudio_JStage.h", "u32 _08;              // _08", "u32 TAdaptor_actor::*mMode; // Native member selector")
    replace("src/JSystem/JStudio_JStage/object-actor.cpp", "// not sure what this bit is\n\tu32 idx = *(u32*)(((u32)adaptor - 1) + _08);",
            "const u32 idx = static_cast<TAdaptor_actor*>(adaptor)->*mMode;")
    replace("src/JSystem/JStudio_JStage/object-actor.cpp", "TVVOutput_ANIMATION_FRAME_(0, 301,", "TVVOutput_ANIMATION_FRAME_(0, &TAdaptor_actor::_12C,")
    replace("src/JSystem/JStudio_JStage/object-actor.cpp", "TVVOutput_ANIMATION_FRAME_(2, 305,", "TVVOutput_ANIMATION_FRAME_(2, &TAdaptor_actor::_130,")
    replace("src/JSystem/JStudio_JStage/object-actor.cpp", "TVVOutput_ANIMATION_FRAME_(-1, 0,", "TVVOutput_ANIMATION_FRAME_(-1, nullptr,")
    replace("src/JSystem/JParticle/JPAEmitter.cpp", "mpUserWork = nullptr;", "mpUserWork = 0;")
    replace("src/JSystem/JUtility/JUTProcBar.cpp", '#include "JSystem/JUtility/JUTProcBar.h"',
            '#include "JSystem/JUtility/JUTProcBar.h"\n#include "p2_memory.h"')
    replace("src/JSystem/JUtility/JUTProcBar.cpp", "((u32)baseAddress - 0x80000000)", "p2_host_to_physical(baseAddress)")
    # Particle resource records contain console scalars, not native objects.
    for path, records in (
        ("include/JSystem/JParticle/JPABlock.h", ("JPADynamicsBlockData", "Data", "JPAKeyBlockData")),
        ("include/JSystem/JParticle/JPAShape.h", ("JPAClrAnmKeyData", "JPABaseShapeData", "JPAChildShapeData", "JPAExTexShapeData", "JPAExtraShapeData")),
        ("include/JSystem/JParticle/JPAResource.h", ("JPAResourceHeader",)),
    ):
        file = destination / path
        text = '#include "p2_endian.h"\n' + file.read_text()
        for record in records:
            pattern = rf"(struct {record} \{{)(.*?)(\n[ \t]*\}};)"
            def particle_record(match):
                body = re.sub(r"\b(u16|s16|u32|f32) (\w+)", r"P2Big<\1> \2", match[2])
                body = body.replace("JGeometry::TVec3f", "P2BigVec3<f32>").replace("JGeometry::TVec3<s16>", "P2BigVec3<s16>")
                return match[1]+body+match[3]
            text, count = re.subn(pattern, particle_record, text, flags=re.S)
            if count != 1: raise RuntimeError(f"Particle disk record patch drift: {record}")
        file.write_text(text)
    replace("include/JSystem/JParticle/JPABlock.h", "*pos = mData->mOffset;", "pos->set(mData->mOffset.x, mData->mOffset.y, mData->mOffset.z);")
    replace("include/JSystem/JParticle/JPABlock.h", "*dir = mData->mVelocity;", "dir->set(mData->mVelocity.x, mData->mVelocity.y, mData->mVelocity.z);")
    replace("include/JSystem/JParticle/JPABlock.h", "const f32* mKeyFrameData;", "const P2Big<f32>* mKeyFrameData;")
    replace("src/JSystem/JParticle/JPAKeyBlock.cpp", "reinterpret_cast<const f32*>", "reinterpret_cast<const P2Big<f32>*>")
    replace("include/JSystem/JParticle/JPAMath.h", "f32 JPACalcKeyAnmValue(f32, u16, const f32*);", "f32 JPACalcKeyAnmValue(f32, u16, const P2Big<f32>*);")
    replace("include/JSystem/JParticle/JPAMath.h", '#include "Dolphin/mtx.h"', '#include "Dolphin/mtx.h"\n#include "p2_endian.h"')
    replace("src/JSystem/JParticle/JPAMath.cpp", "const f32* keyFrameData", "const P2Big<f32>* keyFrameData")
    replace("include/JSystem/JParticle/JPAShape.h", "((f32*)mTexCrdMtxAnmTbl)", "((const P2Big<f32>*)mTexCrdMtxAnmTbl)", count=10)
    replace("src/JSystem/JParticle/JPABaseShape.cpp", "if (i == data[j].index)", "if (j < a2 && i == data[j].index)")
    replace("include/JSystem/JParticle/JPAShape.h", "return &mData->mIndTexMtx[0][0];", "return &mNativeIndTexMtx[0][0];")
    replace("include/JSystem/JParticle/JPAShape.h", "JPAExTexShapeData* mData;", "f32 mNativeIndTexMtx[2][3];\n\tJPAExTexShapeData* mData;")
    replace("src/JSystem/JParticle/JPAExTexShape.cpp", ": mData((JPAExTexShapeData*)data)\n{\n}",
            ": mData((JPAExTexShapeData*)data)\n{\n\tfor (int r=0; r<2; ++r) for (int c=0; c<3; ++c) mNativeIndTexMtx[r][c] = mData->mIndTexMtx[r][c];\n}")
    replace("include/JSystem/JParticle/JPAResource.h", "u16* mTextureIDList;", "const P2Big<u16>* mTextureIDList;")
    loader = "src/JSystem/JParticle/JPAResourceLoader.cpp"
    replace(loader, "(*(type*)((data) + (offset)))", "p2_read_big<type>((data) + (offset))")
    replace(loader, "*(int*)(p1 + 4)", "p2_read_big<u32>(p1 + 4)")
    replace(loader, "*(u32*)(p1 + resourceOffset)", "p2_read_big<u32>(p1 + resourceOffset)")
    replace(loader, "*(u32*)(p1 + resourceOffset + 4)", "p2_read_big<u32>(p1 + resourceOffset + 4)")
    replace(loader, "(u16*)(p1 + resourceOffset + 8)", "reinterpret_cast<const P2Big<u16>*>(p1 + resourceOffset + 8)")
    # Supply the actual DVD read extent to validation before any resource
    # manager walks the pointer-only JPA API used by the original title code.
    for name in ("particle2dMgr.cpp", "particleMgr.cpp", "ebiScreenFileSelect.cpp"):
        file = destination / "src/plugProjectEbisawaU" / name
        text = '#include "p2_particle.h"\n' + file.read_text()
        pattern = r"(^[ \t]*)(const void\*|void\*) file = (JKRDvdToMainRam\((?:path|\"user/Ebisawa/effect/eff2d_file_select.jpc\")[^;]*?), nullptr\);"
        def bounded_particle_load(match):
            indent = match[1]
            return (indent + "u32 nativeJpcSize = 0;\n" + indent + match[2] + " file = " + match[3] + ", &nativeJpcSize);\n" +
                    indent + "const char* nativeJpcError = nullptr;\n" +
                    indent + "if (!p2_validate_jpc(file, nativeJpcSize, &nativeJpcError))\n" +
                    indent + '\tOSPanic(__FILE__, __LINE__, "Invalid particle resource: %s", nativeJpcError);')
        text, count = re.subn(pattern, bounded_particle_load, text, flags=re.M)
        if count != 1: raise RuntimeError(f"Bounded particle load patch drift: {name}")
        file.write_text(text)
    # BFN font scalars and mapping tables stay in console byte order on disk.
    file = destination / "include/JSystem/JUtility/JUTFont.h"
    text = file.read_text()
    begin, end = text.index("struct BlockHeader {"), text.index("struct JUTFont {")
    records = sub_exact(r"\b(u16|u32|u64) (m\w+)", r"P2Big<\1> \2", text[begin:end], 29, "BFN font records")
    text = '#include "p2_endian.h"\n' + text[:begin] + records + text[end:]
    file.write_text(text.replace("convertSjis(int, u16*)", "convertSjis(int, P2Big<u16>*)"))
    file = destination / "src/JSystem/JUtility/JUTResFont.cpp"
    text = '#include "p2_memory.h"\n' + file.read_text()
    text = text.replace("u16* leadingTemp", "P2Big<u16>* leadingTemp").replace("u16* inputLead", "P2Big<u16>* inputLead")
    text = text.replace("JUTReportConsole(", "p2_heap_report(")
    text = text.replace("(GXTexFmt)mGlyphBlocks[glyphBlockIndex]->mTextureFormat",
                        "static_cast<GXTexFmt>(u16(mGlyphBlocks[glyphBlockIndex]->mTextureFormat))")
    file.write_text(text)
    # Native fonts retain disc BFN data in owned main-memory storage. Boot's
    # diagnostic font uses the supplied foreign font; localized messages keep
    # their existing archive selection but no longer require ARAM font paging.
    replace("include/System.h", "struct JUTRomFont* mRomFont;", "struct JUTFont* mRomFont;")
    file = destination / "src/sysGCU/system.cpp"
    file.write_text('#include "p2_font.h"\n' + file.read_text())
    replace("src/sysGCU/system.cpp", "mRomFont = new JUTRomFont(heap);",
            'mRomFont = p2_load_resident_font("/message/font_foreign.szs", "pikmin2main.bfn", heap);\n'
            '\tif (!mRomFont) OSPanic(__FILE__, __LINE__, "Cannot load native boot font");')
    file = destination / "src/sysGCU/messageMgr.cpp"
    message_text = '#include "p2_font.h"\n' + file.read_text()
    pattern = r"(void Mgr::setupFont\(char const\* path, JKRExpHeap\* heap\)\n\{\n).*?^\}"
    replacement = (r'\1' + '\tmFont = p2_load_resident_font(getCurrentFontResName(), path, JKRGetCurrentHeap()); // The US path uses the current (system) heap; `heap` is only for the Japanese cache font.\n'
                   '\tif (!mFont) OSPanic(__FILE__, __LINE__, "Cannot load native message font");\n}')
    message_text, count = re.subn(pattern, replacement, message_text, flags=re.M | re.S)
    if count != 1: raise RuntimeError("Resident message font patch drift")
    file.write_text(message_text)
    # CPU tests use the same constructor, lookup and glyph-selection methods.
    # Drawing aborts; this copy never participates in the native engine build.
    for method, expected in (("setGX", 2), ("drawChar_scale", 1)):
        pattern = rf"((?:void|f32) JUTResFont::{method}\([^\n]*\)\n\{{)\n.*?^\}}"
        text, count = re.subn(pattern, r'\1\n OSPanic(__FILE__, __LINE__, "Font CPU check invoked rendering");\n __builtin_trap();\n}\n', text, flags=re.M | re.S)
        if count != expected: raise RuntimeError(f"Font CPU isolation patch drift: {method}")
    (destination / "checks").mkdir(exist_ok=True)
    (destination / "checks/JUTResFontCpu.cpp").write_text(text)
    # Reset settings live in typed host storage, not a raw GameCube address.
    file = destination / "include/System.h"
    file.write_text('#include "p2_reset.h"\n' + file.read_text())
    replace("include/System.h", "#define RENDER_INFO_STORE ((RenderModeInfo*)DOL_ADDR_LIMIT)",
            "#define RENDER_INFO_STORE (p2_reset_video_state())")
    replace("src/sysGCU/reset.cpp", "OSSetSaveRegion((void*)DOL_ADDR_LIMIT, (void*)(DOL_ADDR_LIMIT + 8));",
            "OSSetSaveRegion(RENDER_INFO_STORE, reinterpret_cast<u8*>(RENDER_INFO_STORE) + sizeof(RenderModeInfo));")
    # Native resource workers can build GD lists concurrently. Acquire on every
    # call (not a static initializer) and retain ownership through final cleanup.
    replace("src/JSystem/J3D/J3DModelData.cpp",
            "\tj3dSys.mTexture           = getMaterialTable().getTexture();\n\tstatic int sInterruptFlag = OSDisableInterrupts();",
            "\tconst BOOL sInterruptFlag = OSDisableInterrupts();\n\tj3dSys.mTexture = getMaterialTable().getTexture();")
    replace("src/JSystem/J3D/J3DShape.cpp",
            "\tstatic s32 sInterruptFlag;\n\tstatic s8 init;\n\n\tif (!init) {\n\t\tsInterruptFlag = OSDisableInterrupts();\n\t\tinit           = true;\n\t}",
            "\tconst BOOL sInterruptFlag = OSDisableInterrupts();")
    packet = "src/JSystem/J3D/J3DPacket.cpp"
    replace(packet, "\tGDPadCurr32();\n\tOSRestoreInterrupts(sInterruptFlag);\n\tmSize = sGDLObj.data - sGDLObj.begin;\n\tGDFlushCurrToMem();\n\t__GDCurrentDL = nullptr;",
            "\tGDPadCurr32();\n\tmSize = sGDLObj.data - sGDLObj.begin;\n\tGDFlushCurrToMem();\n\t__GDCurrentDL = nullptr;\n\tOSRestoreInterrupts(sInterruptFlag);")
    replace(packet, "\tOSRestoreInterrupts(sInterruptFlag);\n\t__GDCurrentDL = nullptr;",
            "\t__GDCurrentDL = nullptr;\n\tOSRestoreInterrupts(sInterruptFlag);")
    # Native assertions fail immediately with host diagnostics. Do not install
    # PPC exception handlers or run the console register/framebuffer monitor.
    exception = "src/JSystem/JUtility/JUTException.cpp"
    file = destination / exception
    file.write_text('#include "p2_panic.h"\n' + file.read_text())
    for kind in ("DSI", "ISI", "PROGRAM", "ALIGNMENT", "PROTECTION"):
        replace(exception, f"\tOSSetErrorHandler(OS_ERROR_{kind}, (OSErrorHandler)errorHandler);\n", "")
    replace(exception, "\t\tOSResumeThread(sErrorManager->mThread);",
            "\t\t// Native panic reports on the failing thread; no console monitor is resumed.")
    def native_exception_body(path, method_signature, body):
        file = destination / path
        pattern = re.escape(method_signature) + r"\n\{\n.*?^\}"
        text, count = re.subn(pattern, lambda m: method_signature + "\n{\n" + body + "\n}",
                              file.read_text(), flags=re.M | re.S)
        if count != 1: raise RuntimeError(f"Native exception patch drift: {method_signature}")
        file.write_text(text)
    native_exception_body(exception, "void* JUTException::run()",
        '\tOSPanic(__FILE__, __LINE__, "Console exception monitor invoked in native port");\n\treturn nullptr;')
    native_exception_body(exception, "void JUTException::errorHandler(OSError error, OSContext* context, u32 p3, u32 p4)",
        '\tOSPanic(__FILE__, __LINE__, "Console error %u context %p details %08x %08x", unsigned(error), context, p3, p4);')
    native_exception_body(exception, "void JUTException::panic_f_va(const char* fileName, int lineNumber, const char* format, va_list* args)",
        '\tp2_panic_v(fileName, lineNumber, format, args);')
    native_exception_body(exception, "void JUTException::setFPException(u32 enableBits)",
        '\tif (enableBits) OSPanic(__FILE__, __LINE__, "PPC floating-point trap mask is unsupported: %08x", enableBits);')
    file = destination / "src/sysGCU/system.cpp"
    file.write_text('#include "p2_panic.h"\n' + file.read_text())
    native_exception_body("src/sysGCU/system.cpp",
        "static void kando_panic_f_va(bool r3, const char* file, int line, const char* format, va_list* args)",
        '\tp2_panic_v(file, line, format, args);')
    # The sound system is created as on console (createSoundSystem and
    # loadSoundResource run unchanged): gameplay dereferences its singletons.
    # Playback below JAIBasic is silent (src/audio_silent.cpp).
    replace("src/sysGCU/system.cpp", "if (sysif = PSSystem::spSysIF)", "if (P2_AUDIO_ENABLED && (sysif = PSSystem::spSysIF))")
    # Audit only the startup/title UI here. Gameplay audio has separate owners.
    ui_audio = {
        "ebiScreenOmake.cpp": 3, "ebiScreenFileSelect_Mgr.cpp": 13,
        "ebiScreenE3TitleMenu.cpp": 3, "ebiScreenOmakeGame.cpp": 2,
        "ebiScreenFileSelect.cpp": 4, "ebiOptionMgr.cpp": 1,
        "ebiScreenProgre.cpp": 3, "ebiScreenOmakeCardE.cpp": 4,
        "ebiScreenPushStart.cpp": 1, "ebiScreenMemoryCard.cpp": 33,
        "ebiOmakeMgr.cpp": 7, "ebiScreenSaveMenu.cpp": 8,
        "ebiScreenOption.cpp": 15, "ebiScreenTitleMenu.cpp": 4,
    }
    audio_calls = {f"src/plugProjectEbisawaU/{name}": count for name, count in ui_audio.items()}
    audio_calls.update({"src/sysGCU/bootSection.cpp": 3, "src/sysGCU/titleSection.cpp": 3,
                       "src/sysGCU/messageSequence.cpp": 1, "src/sysGCU/demoSection.cpp": 1,
                       "src/sysGCU/movieMessage.cpp": 3, "include/Morimura/VsSelect.h": 2, "src/utilityU/menu.cpp": 2})
    shared_ui_audio = {
        "plugProjectKonoU": {
            "khCaveResult.cpp": 17, "khDayEndResult.cpp": 28,
            "khFinalFloor.cpp": 2, "khFinalResult.cpp": 3, "khPayDept.cpp": 1,
            "khReadyGo.cpp": 2, "newGame2DMgr.cpp": 1, "khWorldMap.cpp": 6,
        },
        "plugProjectMorimuraU": {
            "challengeResult2D.cpp": 17, "challengeSelect2D.cpp": 11,
            "dayEndCount.cpp": 1, "hiScore2D.cpp": 4, "hurryUp2D.cpp": 1,
            "vsSelect2D.cpp": 16, "zukan2D.cpp": 21,
        },
        "plugProjectOgawaU": {"ogSE.cpp": 33},
    }
    for directory, files in shared_ui_audio.items():
        audio_calls.update({f"src/{directory}/{name}": count for name, count in files.items()})
    for path, expected in audio_calls.items():
        file = destination / path
        text, count = re.subn(r"(?m)^([ \t]*)(PSSystem::spSysIF->playSystemSe\([^\n]*\));$",
                             r"\1P2_AUDIO_ONLY(\2);", file.read_text())
        if count != expected: raise RuntimeError(f"Startup sound call patch drift: {path}: {count}")
        file.write_text(text)
    def audio_span(path, pattern, expected=1):
        file = destination / path
        text, count = re.subn(pattern, lambda m: "#if P2_AUDIO_ENABLED\n" + m[0] + "\n#endif", file.read_text(), flags=re.M | re.S)
        if count != expected: raise RuntimeError(f"Startup audio block patch drift: {path}: {count}")
        file.write_text(text)
    # Registered shared screens retain visual/state work but never access an
    # unconstructed sound engine in the disabled-audio build.
    def audio_method(path, method):
        file = destination / path
        pattern = rf"(void {re.escape(method)}\([^\n]*\)\n\{{\n)(.*?)(^\}})"
        text, count = re.subn(pattern, lambda m: m[1] + "#if P2_AUDIO_ENABLED\n" + m[2] + "#endif\n" + m[3],
                              file.read_text(), flags=re.M | re.S)
        if count != 1: raise RuntimeError(f"Shared UI sound method patch drift: {method}")
        file.write_text(text)
    kono = "src/plugProjectKonoU/"
    audio_method(kono + "khDayEndResult.cpp", "ObjDayEndResultIncP::callDecPSE")
    audio_method(kono + "khFinalFloor.cpp", "ObjFinalFloor::stopSound")
    audio_method(kono + "khFinalFloor.cpp", "ObjFinalFloor::restartSound")
    replace(kono + "khFinalFloor.cpp", "\tstartBGM();", "\tP2_AUDIO_ONLY(startBGM());", count=2)
    audio_span("include/kh/khFinalFloor.h", r"^\t\tPSStart2DStream\(.*?^\t\tscene->onStartMainSeq\(\);")
    for name, expected in (("khPayDept.cpp", 4), ("khReadyGo.cpp", 2), ("khWinLose.cpp", 3)):
        file = destination / (kono + name)
        text, count = re.subn(r"(?m)^([ \t]*)(PS(?:Start2DStream|Stop2DStream|StartChallengeTimeUpStream|MuteOffSE_on2D)\([^\n]*\));$",
                              r"\1P2_AUDIO_ONLY(\2);", file.read_text())
        if count != expected: raise RuntimeError(f"Shared UI stream patch drift: {name}: {count}")
        file.write_text(text)
    audio_span(kono + "khWinLoseReason.cpp", r"^\t\tPSStart2DStream\(streamID\);.*?^\t\tscene->stopAllSound\(2\);")
    file = destination / (kono + "khWorldMap.cpp")
    text, count = re.subn(r"(?m)^([ \t]*)(PSMGetWorldMapRocket\(\)->(?:stateChange|startRocketSE)\([^\n]*\));$",
                          r"\1P2_AUDIO_ONLY(\2);", file.read_text())
    if count != 15: raise RuntimeError("World-map sound callback patch drift")
    file.write_text(text)
    morimura = "src/plugProjectMorimuraU/"
    for name, line in (("dayEndCount.cpp", 228), ("hurryUp2D.cpp", 210)):
        call = f"P2ASSERTLINE({line}, PSSystem::spSysIF);"
        replace(morimura + name, call, f"P2_AUDIO_ONLY({call});")
    # Each handle is local only to the sound play/pan block; the following
    # Pikmin position, timer and animation-state changes remain live.
    audio_span(morimura + "challengeSelect2D.cpp", r"^\t{4}JAISound\* sound = .*?^\t{4}\}")
    audio_span(morimura + "challengeSelect2D.cpp", r"^\t{5}JAISound\* sound\n.*?^\t{5}\}")
    ogawa = "src/plugProjectOgawaU/"
    for method in ("setChimeNoon", "setVsWin1P", "setVsWin2P", "setVsDraw"):
        audio_method(ogawa + "ogSE.cpp", "Sound::" + method)
    audio_span(ogawa + "ogObjKantei.cpp", r"^\t{5}PSGame::SeMgr\* seMgr = .*?^\t{5}seMgr->playMessageVoice\([^\n]*;", 3)
    audio_span(ogawa + "ogObjKantei.cpp", r"^\t{2}PSSystem::SceneMgr\* mgr = .*?^\t{2}PSSystem::checkChildScene\(scene\)->startMainSeq\(\);")
    audio_span(ogawa + "ogObjSpecialItem.cpp", r"^\t{2}PSSystem::SceneMgr\* mgr = .*?^\t{2}scene->startMainSeq\(\);")
    audio_span(ogawa + "ogOtakaraSensor.cpp", r"^\t{4}PSSystem::SceneMgr\* mgr = .*?^\t{4}\}")
    for call in ("PSStartTresureLaderNoiseSE(mState, mNoiseLevel, mCurrReactionLevel);",
                 "PSStartTreasureLaderSE(mCurrReactionLevel);"):
        replace(ogawa + "ogOtakaraSensor.cpp", call, "P2_AUDIO_ONLY(" + call + ");")
    ground = ogawa + "ogSceneGround.cpp"
    audio_span(ground, r"^\t{2}PSSystem::SceneMgr\* mgr = .*?^\t{2}PSM::Scene_Ground\* scene = [^\n]*;")
    for call in ("scene->fadeMainBgm(0.0f, 0, PSM::Scene_Ground::GroundTime_Off);", "scene->jumpMainBgm(1);"):
        replace(ground, call, "P2_AUDIO_ONLY(" + call + ");")
    audio_span(ground, r"^\t{2}f32 cTime = timemgr->getRealDayTime\(\);.*?^\t{2}\}(?=\n\t\}\n\})")
    audio_method("src/sysGCU/sysShapeAnimation.cpp", "AnimMgr::registerSoundViewer")
    thp = "src/sysGCU/pikmin2THPPlayer.cpp"
    replace(thp, "PSM::sTHPDinamicsProc.setSetting((PSM::THP_ID)data->mThpID);",
            "P2_AUDIO_ONLY(PSM::sTHPDinamicsProc.setSetting((PSM::THP_ID)data->mThpID));")
    replace(thp, "PSStop2DStream();", "P2_AUDIO_ONLY(PSStop2DStream());")
    audio_span(thp, r"^\t\tf32 vol = 127.0f .*?^\t\tTHPPlayerSetVolume\(vol, 0\);")
    replace(thp, "\t\tTHPPlayerPlay();", "#if !P2_AUDIO_ENABLED\n\t\tTHPPlayerSetVolume(0.0f, 0);\n#endif\n\t\tTHPPlayerPlay();")
    # Camera FOV shares a translation unit with the sound singleton. Keep the
    # camera tuning value available without constructing the disabled engine's
    # global parameter object (or retaining its sound-table setter).
    creature_prm = "src/utilityU/PSMainSide_CreaturePrm.cpp"
    replace(creature_prm, "CreaturePrm sInsReal;", "#if P2_AUDIO_ENABLED\nCreaturePrm sInsReal;\n#endif")
    replace(creature_prm, "mPersp.set(1.0f, 400.0f, 0.8f, 700.0f, 0.0f);",
            "P2_AUDIO_ONLY(mPersp.set(1.0f, 400.0f, 0.8f, 700.0f, 0.0f));")
    # Demo exit and text voice hooks must not instantiate or query sound state
    # when audio is disabled. Visual update and message progression stay intact.
    replace("src/sysGCU/demoSection.cpp", "PSMGetSceneMgrCheck()->deleteCurrentScene();",
            "P2_AUDIO_ONLY(PSMGetSceneMgrCheck()->deleteCurrentScene());")
    for method in ("doCharacterSE", "doCharacterSEStart", "doCharacterSEEnd", "doFastForwardSE"):
        file = destination / "src/sysGCU/windowMessage.cpp"
        pattern = rf"(void TSequenceProcessor::{method}\([^\n]*\)\n\{{\n)(.*?)(^\}})"
        text, count = re.subn(pattern, lambda m: m[1] + "#if P2_AUDIO_ENABLED\n" + m[2] + "#endif\n" + m[3],
                              file.read_text(), flags=re.M | re.S)
        if count != 1: raise RuntimeError(f"Message voice patch drift: {method}")
        file.write_text(text)
    boot = "src/sysGCU/bootSection.cpp"
    audio_span(boot, r"^\t{3}PSM::Scene_Global\* scene = .*?scene->startGlobalStream\([^\n]+;")
    audio_span(boot, r"^\t{2}PSSystem::SceneMgr\* mgr = .*?scene->startGlobalStream\([^\n]+;")
    file = destination / boot
    text, count = re.subn(r"(^\tPSSystem::SceneMgr\* mgr = .*?^\tJAISound\* handle = [^\n]+;)",
        lambda m: "#if P2_AUDIO_ENABLED\n" + m[0] + "\n#else\n\tJAISound* handle = nullptr; // No greeting stream to wait for.\n#endif", file.read_text(), flags=re.M | re.S)
    if count != 1: raise RuntimeError("Boot greeting wait patch drift")
    file.write_text(text)
    title = "src/sysGCU/titleSection.cpp"
    audio_span(title, r"^\tPSSystem::SceneMgr\* mgr = .*?^\tmgr->deleteCurrentScene\(\);")
    audio_span(title, r"^\tPSGame::SceneInfo info;.*?^\tmgr2->mScenes->mChild->scene1stLoadSync\(\);")
    # Every span contains only scene lookup, sequence selection and fade/volume
    # arithmetic. Menu/THP state transitions remain outside the guarded spans.
    audio_span(title, r"^[ \t]+mgr[ \t]*= PSSystem::getSceneMgr\(\);.*?^[ \t]+seq->(?:startSeq|stopSeq)\([^\n]*;", 7)
    audio_span(title, r"^[ \t]+PSSystem::SeqBase\* seq = PSSystemGetSeqCheck\([^\n]+;.*?^[ \t]+seq->stopSeq\([^\n]*;", 2)
    replace(title, "PSM::ObjMgr::newInstance();", "P2_AUDIO_ONLY(PSM::ObjMgr::newInstance());")
    replace(title, "PSMGetSceneMgrCheck()->doStartMainSeq();", "P2_AUDIO_ONLY(PSMGetSceneMgrCheck()->doStartMainSeq());")
    replace("include/Title.h", "PSSystem::Scene* scene = PSSystem::getChildSceneCheck(PSMGetSceneMgrCheck());\n\t\tPSSystem::getSeqFromScene(scene, BGM_MainTheme)->startSeq();",
            "P2_AUDIO_ONLY(PSSystem::Scene* scene = PSSystem::getChildSceneCheck(PSMGetSceneMgrCheck());\n\t\tPSSystem::getSeqFromScene(scene, BGM_MainTheme)->startSeq());")
    replace("src/sysGCU/section.cpp", "PSMGetSceneMgrCheck()->doStopMainSeqCheck(timer);", "P2_AUDIO_ONLY(PSMGetSceneMgrCheck()->doStopMainSeqCheck(timer));")
    common = "src/sysGCU/commonSaveData.cpp"
    for mode in ("SM_Mono", "SM_Stereo", "SM_SurroundSound"):
        replace(common, f"JAIGlobalParameter::setParamSoundOutputMode({mode});", f"P2_AUDIO_ONLY(JAIGlobalParameter::setParamSoundOutputMode({mode}));")
    replace(common, "OSSetSoundMode(OS_SOUND_MODE_MONO);", "P2_AUDIO_ONLY(OSSetSoundMode(OS_SOUND_MODE_MONO));")
    replace(common, "OSSetSoundMode(OS_SOUND_MODE_STEREO);", "P2_AUDIO_ONLY(OSSetSoundMode(OS_SOUND_MODE_STEREO));", count=2)
    for method, field in (("setBgmVolume", "mMusicVol"), ("setSeVolume", "mSeVol")):
        file = destination / common
        pattern = rf"(void Mgr::{method}\(f32 volume\)\n\{{\n)(.*?)(^\}})"
        fallback = f'\tif (!(volume >= 0.0f && volume <= 1.0f)) OSPanic(__FILE__, __LINE__, "Invalid sound volume");\n\t{field} = ROUND_F32_TO_U8(volume * 255.0f);\n'
        text, count = re.subn(pattern, lambda m: m[1]+"#if P2_AUDIO_ENABLED\n"+m[2]+"#else\n"+fallback+"#endif\n"+m[3], file.read_text(), flags=re.M | re.S)
        if count != 1: raise RuntimeError(f"Sound preference patch drift: {method}")
        file.write_text(text)
    audio_span("src/sysGCU/reset.cpp", r"^\t{4}if \(PSSystem::spSysIF\) \{.*?^\t{4}\}")
    file = destination / "src/sysGCU/reset.cpp"
    text, count = re.subn(r"(bool ResetManager::isSoundSystemStopped\(\)\n\{\n)(.*?)(^\})",
        lambda m: m[1]+"#if P2_AUDIO_ENABLED\n"+m[2]+"#else\n\treturn true;\n#endif\n"+m[3], file.read_text(), flags=re.M | re.S)
    if count != 1: raise RuntimeError("Reset audio wait patch drift")
    file.write_text(text)
    # Movie playback retains visual/message/state work without constructing
    # sound objects. parse(true) already uses 0x40 (allow missing objects), and
    # subsequent parse(false) uses 0x30 (reuse objects / skip absent objects).
    movie = "src/sysGCU/moviePlayer.cpp"
    replace(movie, "PSMCancelToPauseOffMainBgm();", "P2_AUDIO_ONLY(PSMCancelToPauseOffMainBgm());", count=2)
    replace(movie, "mDemoPSM                                 = new PSM::Demo;",
            "P2_AUDIO_ONLY(mDemoPSM = new PSM::Demo;);")
    audio_span(movie, r"^\tPSSystem::SysIF\* sysif.*?^\tmStudioFactory->appendCreateObject\(mPikminCreateObjectAudio\);")
    audio_span(movie, r"^\t\tPSM::DemoArg arg.*?^\t\tmDemoPSM->onDemoTop\(\);")
    audio_span(movie, r"^\tPSM::DemoArg arg.*?^\tmDemoPSM->init\([^\n]*;")
    for call, count in (("onDemoFadeoutStart(30)", 2), ("onDemoEnd()", 1),
                        ("onDemoTop()", 2), ("onMessageEnd(mMessageEndCount)", 1)):
        replace(movie, "mDemoPSM->" + call + ";", "P2_AUDIO_ONLY(mDemoPSM->" + call + ";);", count=count)
    # skip() deleted '#' objects but left them linked; the finishing forward()
    # then dispatched through the dead object. The console's freed memory still
    # held a harmless vtable; clang leaves the abstract one (pure virtual abort).
    replace(movie, """	for (JGadget::TLinkList<JStudio::stb::TObject, -12>::iterator it = objects.begin(); it != objects.end(); ++it) {
		char* id = (char*)it->mIDString;
		if (id[0] == '#') {
			delete it.operator->();
		}
	}""", """	for (JGadget::TLinkList<JStudio::stb::TObject, -12>::iterator it = objects.begin(); it != objects.end();) {
		JStudio::stb::TObject* object = it.operator->();
		char* id                      = (char*)object->mIDString;
		if (id[0] == '#') {
			it = objects.Erase(object);
			delete object;
		} else {
			++it;
		}
	}""")
    # Produce the VI event on the render thread before consuming JUTVideo's
    # post-retrace queue. Aurora does not emulate a hardware interrupt ticker.
    replace("src/JSystem/JFramework/JFWDisplay.cpp",
            "\t\tdo {\n\t\t\tif (!OSReceiveMessage(JUTVideo::getManager()->getMessageQueue(), &msg, OS_MESSAGE_BLOCK)) {",
            "\t\tdo {\n\t\t\tVIWaitForRetrace();\n\t\t\tif (!OSReceiveMessage(JUTVideo::getManager()->getMessageQueue(), &msg, OS_MESSAGE_BLOCK)) {")
    file = destination / "src/JSystem/JFramework/JFWDisplay.cpp"
    file.write_text('#include "p2_renderer.h"\n' + file.read_text())
    replace("src/JSystem/JFramework/JFWDisplay.cpp",
            "\tJUTVideo::getManager()->waitRetraceIfNeed();",
            "\tJUTVideo::getManager()->waitRetraceIfNeed();\n\tp2_renderer_begin_frame();")
    replace("src/JSystem/JFramework/JFWDisplay.cpp",
            "\tprevFrame = retrace_cnt;",
            "\tprevFrame = retrace_cnt;\n\tp2_renderer_end_frame();")
    # Replace the console alarm/suspend pair with a native timed wait. The
    # draw-done path still drains GX; the removed watchdog reads GC registers
    # and cannot diagnose a Metal device. No JFW alarms remain to cancel.
    file = destination / "src/JSystem/JFramework/JFWDisplay.cpp"
    text = '#include "p2_memory.h"\n' + file.read_text()
    for method_signature, body in (
        (r"void JFWDisplay::threadSleep\(OSTime time\)", "\tp2_thread_sleep_ticks(time);"),
        (r"void JFWDrawDoneAlarm\(\)", "\tGXDrawDone();")):
        text, count = re.subn(r"(" + method_signature + r"\n\{\n).*?^\}",
                             lambda m: m[1] + body + "\n}", text, flags=re.M | re.S)
        if count != 1: raise RuntimeError("Native display wait patch drift")
    old = "\tfor (JSUPtrLink* link = JFWAlarm::sList.mHead; link != nullptr; link = link->getNext()) {\n\t\t((JFWAlarm*)link->mValue)->cancelAlarm();\n\t}\n"
    if text.count(old) != 1: raise RuntimeError("Native display alarm cleanup drift")
    text = text.replace(old, "\t// Native display waits do not allocate console alarms.\n")
    file.write_text(text)
    delegates = re.compile(r"(Delegate(?:\d+)?<([\w:]+)(?:,[^<>]*)?>\s*(?:\w+\s*)?\(\s*this,\s*)&?(\w+)(\s*\))")
    for file in list((destination / "src").rglob("*.cpp")) + list((destination / "include").rglob("*.h")):
        text = file.read_text()
        updated = delegates.sub(lambda m: m[1] + "&" + m[2] + "::" + m[3] + m[4], text)
        # Keep the game's 15-bit MSL RNG distinct from host libc/Aurora RNG.
        # The original randFloat divides by 32768; host rand can exceed that
        # by 65536x, producing invalid shuffle/array indices during boot.
        updated = re.sub(r"\b(srand|rand)(\s*\()", lambda m: "p2_game_" + m[1] + m[2], updated)
        if updated != text:
            file.write_text(updated)
    # ID32 is a numeric four-character code, not a native integer byte overlay.
    # Text uses display order; its historical binary wire format is low byte first.
    id_file = destination / "src/sysCommonU/id32.cpp"
    text = '#include "p2_endian.h"\n' + id_file.read_text()
    for method_signature, body in (
        (r"void ID32::updateID\(\)", "\tmId.mIntView = p2_read_big<u32>(mStringID);"),
        (r"void ID32::updateString\(\)", "\tsprint(mStringID);"),
        (r"void ID32::read\(Stream& stream\)",
         "\tif (stream.mMode == STREAM_MODE_TEXT) {\n\t\tmId.mIntView = p2_read_big<u32>(stream.getNextToken());\n\t} else {\n\t\tmId.mIntView = 0;\n\t\tfor (unsigned i = 0; i < 4; ++i) mId.mIntView |= u32(stream.readByte()) << (8 * i);\n\t}\n\tupdateString();"),
        (r"void ID32::write\(Stream& stream\)",
         "\tif (stream.mMode == STREAM_MODE_TEXT) {\n\t\tchar str[5];\n\t\tsprint(str);\n\t\tstream.printf(\"{%s} \", str);\n\t} else {\n\t\tfor (unsigned i = 0; i < 4; ++i) stream.writeByte(u8(mId.mIntView >> (8 * i)));\n\t}")):
        text, count = re.subn(r"(" + method_signature + r"\n\{\n).*?^\}",
                             lambda match: match[1] + body + "\n}", text, flags=re.M | re.S)
        if count != 1: raise RuntimeError("Native ID32 conversion patch drift")
    id_file.write_text(text)
    # Aurora's native PAD API appends extButton and strides sixteen bytes.
    replace("include/Dolphin/pad.h", "} PADStatus;", "\tu32 extButton; // Native Aurora extension; preserve four-channel array stride.\n} PADStatus;")
    message_mgr = destination / "src/sysGCU/messageMgr.cpp"
    message_mgr.write_text('#include "p2_message_resource.h"\n' + message_mgr.read_text())
    replace("src/sysGCU/messageMgr.cpp", "parse.parse(file, 0)", "(p2_validate_message_resource(file, arc->getResSize(file), false) && parse.parse(file, 0))", count=2)
    replace("src/sysGCU/messageMgr.cpp", "parse.parse(file, 0x20)", "(p2_validate_message_resource(file, arc->getResSize(file), true) && parse.parse(file, 0x20))")
    # Factory cleanup must unlink each resource before deleting it.
    replace("include/JSystem/JGadget/linklist.h", "T* item = &this->front();\n\t\t\tDo_destroy(item);", "T* item = &this->front();\n\t\t\tthis->Erase(item);\n\t\t\tDo_destroy(item);")
    # Intrusive-list offsets name GameCube member positions; each listed type reports its host position instead.
    replace('include/JSystem/JGadget/linklist.h', 'template <typename T, int I>\nstruct TLinkList : public TNodeLinkList {',
            'template <typename T, int I> struct TLinkListOffset {\n\tstatic ptrdiff_t get() { return I; }\n};\n\ntemplate <typename T, int I>\nstruct TLinkList : public TNodeLinkList {', count=1)
    replace('include/JSystem/JGadget/linklist.h', '(element) - I)', '(element) - TLinkListOffset<T, I>::get())', count=2)
    replace('include/JSystem/JGadget/linklist.h', '(node) + I)', '(node) + TLinkListOffset<T, I>::get())', count=2)
    replace('include/JSystem/JStudio/stb.h', 'typedef JGadget::TLinkList<TObject, -(int)sizeof(TObject_ID)> LinkList;', 'typedef JGadget::TLinkList<TObject, -12> LinkList;', count=1)
    for path, type, offset, member in (
        ('include/JSystem/JStudio/stb.h', 'JStudio::stb::TObject', -12, 'mNode'),
        ('include/JSystem/JStudio/fvb.h', 'JStudio::fvb::TObject', -12, 'mNode'),
        ('include/JSystem/JStudio/TCreateObject.h', 'JStudio::TCreateObject', -4, 'mLinkListNode'),
        ('include/JSystem/JUtility/JUTConsole.h', 'JUTConsole', -0x18, 'mListNode')):
        text = (destination / path).read_text()
        end = text.rindex('#endif')
        (destination / path).write_text(text[:end] +
            '#pragma clang diagnostic push\n#pragma clang diagnostic ignored "-Winvalid-offsetof"\n'
            'template <> struct JGadget::TLinkListOffset<%s, %d> {\n\tstatic ptrdiff_t get() { return -(ptrdiff_t)offsetof(%s, %s); }\n};\n'
            '#pragma clang diagnostic pop\n\n' % (type, offset, type, member) + text[end:])
    # BMG/BMC resources retain their GameCube byte layout on a native host.
    message_data = destination / "include/JSystem/JMessage/data.h"
    text = '#include "p2_endian.h"\n' + message_data.read_text()
    text = text.replace('u32 getSize() const { return *(u32*)(get() + 0x4); }',
                        'u32 getSize() const { return *(u32*)(get() + 0x8); }')
    text = re.sub(r'\*\((u16|u32)\*\)\(get\(\) \+ (0x[0-9A-Fa-f]+)\)',
                  r'p2_read_big<\1>(get() + \2)', text)
    message_data.write_text(text)
    replace("include/JSystem/JMessage/TResource.h", "mMessages + *(int*)entry", "mMessages + p2_read_big<u32>(entry)")
    message_resource = "src/JSystem/JMessage/resource.cpp"
    replace(message_resource, "&data::ga4cSignature, sizeof(data::ga4cSignature)", '\"MESG\", 4')
    replace(message_resource, "&data::ga4cSignature_color, sizeof(data::ga4cSignature_color)", '\"MGCL\", 4')
    replace(message_resource, "(char*)&header[2]", "(const char*)header + 8", count=2)
    replace(message_resource, "const u32*", "const P2Big<u32>*", count=4)
    replace(message_resource, "(u32*)mMessageID.getContent()", "(const P2Big<u32>*)mMessageID.getContent()")
    replace(message_resource, "(u32*)(first + mMessageID.get_number())", "first + mMessageID.get_number()")
    replace(message_resource, "(const void*)((uintptr_t)pData + ((u32*)pData)[1])", "block.getNext()")
    replace(message_resource, "((u32*)pData)[1]", "block.get_size()")
    replace(message_resource, "((int*)pData)[0]", "block.get_type()")
    # Tag payloads can be unaligned; never load native integers from them.
    message_processor = "src/JSystem/JMessage/processor.cpp"
    replace(message_processor, "*(int*)entry", "p2_read_big<u32>(entry)")
    replace(message_processor, "*(u32*)p2", "p2_read_big<u32>(p2)", count=3)
    replace(message_processor, "*(u16*)(data)", "p2_read_big<u16>(data)", count=2)
    replace(message_processor, "JGadget::binary::TParseValue<JGadget::binary::TParseValue_endian_big_<u16> >::parse(puOffset)", "p2_read_big<u16>(puOffset)")
    replace(message_processor, "JGadget::binary::TParseValue<JGadget::binary::TParseValue_endian_big_<u32> >::parse(puOffset)", "p2_read_big<u32>(puOffset)")
    replace(message_processor, "((u16*)processor->mProc.mBranchProc.mTargetAddr)[p2]", "p2_read_big<u16>((const u8*)processor->mProc.mBranchProc.mTargetAddr + 2 * p2)")
    # The decomp's jump-union alias only overlapped this branch pointer on PPC.
    replace(message_processor, "((u32*)processor->mProc.mJumpProc.mTarget)[p2]", "p2_read_big<u32>((const u8*)processor->mProc.mBranchProc.mTargetAddr + 4 * p2)", count=2)
    replace("src/sysGCU/messageRendering.cpp", "*static_cast<const u16*>(p1)", "p2_read_big<u16>(p1)")
    replace("src/sysGCU/messageRendering.cpp", "*(u16*)data", "p2_read_big<u16>(data)", count=2)
    # Card commands are native objects, not 32-byte PowerPC blobs. In particular
    # a derived scalar can reuse base tail padding while a pointer aligns to 8.
    # Transfer fields explicitly into the queue's stable command type.
    card_header = "include/MemoryCardMgr.h"
    replace(card_header, "struct MemoryCardMgrCommandBase {",
            "struct MemoryCardMgrCommand;\nstruct MemoryCardMgrCommandBase {")
    replace(card_header, "virtual u32 getClassSize() = 0;",
            "virtual void copyTo(MemoryCardMgrCommand&) const = 0;\n\tvirtual u32 getClassSize() = 0;")
    replace(card_header, ": MemoryCardMgrCommandBase(val)\n\t{",
            ": MemoryCardMgrCommandBase(val)\n\t    , mData{}\n\t{\n\t\tmData.dataView = nullptr;")
    replace(card_header, "virtual u32 getClassSize() { return sizeof(MemoryCardMgrCommand); }",
            "void copyTo(MemoryCardMgrCommand& out) const override { out.mFlag = mFlag; out.mData = mData; }\n\tvirtual u32 getClassSize() { return sizeof(MemoryCardMgrCommand); }")
    for command_type, fields in (
        ("MgrCommandLoadGameOption", "out.mData.byteView = mLoadLanguage;"),
        ("MgrCommandCopyPlayer", "out.mData.shortView[0] = mFileIndex1; out.mData.shortView[1] = mFileIndex2;"),
        ("MgrCommandPlayerNo", "out.mData.intView = mFileIndex;"),
        ("MgrCommandGetPlayerHeader", "out.mData.dataView = mPlayerInfo;")):
        old = "virtual u32 getClassSize() { return sizeof(" + command_type + "); }"
        replace("include/Game/MemoryCard/Mgr.h", old,
            "void copyTo(MemoryCardMgrCommand& out) const override { out.mFlag = mFlag; out.mData.dataView = nullptr; " + fields + " }\n\t" + old)
    replace("src/sysGCU/memoryCard.cpp", "command->getClassSize() <= 0x20",
            "command && command->getClassSize() <= sizeof(MemoryCardMgrCommand)")
    replace("src/sysGCU/memoryCard.cpp",
            "u8* base = (u8*)this + j * sizeof(MemoryCardMgrCommand);\n\t\t\t\tmemcpy(base++ + 4, (void*)command, sizeof(MemoryCardMgrCommand));",
            "command->copyTo(cmd[j]);")
    # Sound objects compile natively so gameplay creatures own real PSM objects;
    # audio stays silent below them. Loader/DVD callback user data carries
    # pointers, so it is pointer-sized. Bank relocation stores pointers in
    # 32-bit fields and cannot work natively: it panics until audio is ported.
    replace("include/JSystem/JAudio/JAS/JASResArcLoader.h", "typedef void (*LoadCallback)(u32, u32);", "typedef void (*LoadCallback)(u32, uintptr_t);")
    replace("include/JSystem/JAudio/JAS/JASResArcLoader.h", "\tu32 mCallbackArg;       // _14", "\tuintptr_t mCallbackArg; // _14")
    replace("include/JSystem/JAudio/JAS/JASResArcLoader.h", "LoadCallback callback, u32 cbArg);", "LoadCallback callback, uintptr_t cbArg);")
    replace("include/JSystem/JAudio/JAS/JASDvd.h", "typedef void (*JASDvdCallback)(u32);", "typedef void (*JASDvdCallback)(uintptr_t);")
    replace("include/JSystem/JAudio/JAS/JASDvd.h", "\tu32 _00;            // _00\n\tu32* _04;", "\tuintptr_t _00;      // _00\n\tu32* _04;")
    replace("include/JSystem/JAudio/JAS/JASDvd.h", "void checkPassDvdT(u32, u32*, JASDvdCallback);", "void checkPassDvdT(uintptr_t, u32*, JASDvdCallback);")
    replace("include/PSSystem/PSSeq.h", "static void loadedCallback(u32, u32);", "static void loadedCallback(u32, uintptr_t);")
    replace("src/plugProjectHikinoU/PSSeq.cpp", "void SeqHeap::loadedCallback(u32 arg1, u32 arg2)", "void SeqHeap::loadedCallback(u32 arg1, uintptr_t arg2)")
    replace("src/plugProjectHikinoU/PSSeq.cpp", "loadedCallback(size, (u32)sound->mSeqHeap);", "loadedCallback(size, (uintptr_t)sound->mSeqHeap);")
    replace("src/plugProjectHikinoU/PSSeq.cpp", "&loadedCallback, (u32)this);", "&loadedCallback, (uintptr_t)this);")
    replace("include/PSSystem/WaveScene.h", "static void waveLoadCallback(u32);", "static void waveLoadCallback(uintptr_t);")
    replace("src/plugProjectHikinoU/PSBnkMgr.cpp", "void WaveScene::WaveArea::waveLoadCallback(u32 areaLoader)", "void WaveScene::WaveArea::waveLoadCallback(uintptr_t areaLoader)")
    replace("src/plugProjectHikinoU/PSBnkMgr.cpp", "JASDvd::checkPassDvdT((u32)loader, nullptr, &waveLoadCallback);", "JASDvd::checkPassDvdT((uintptr_t)loader, nullptr, &waveLoadCallback);")
    # Sequence registers hold 32 bits, so objects are stored as handles into a
    # fixed table (no allocation: callers run on the audio thread).
    native_body("src/plugProjectHikinoU/PSSystemIF.cpp", "setObject", """
\tstatic const u32 kSlots = 1 << 16;
\tu32 slot = u32((uintptr_t(p2) >> 4) * 0x9E3779B1u) & (kSlots - 1);
\tfor (u32 probe = 0; sObjectSlots[slot] != p2; slot = (slot + 1) & (kSlots - 1)) {
\t\tif (!sObjectSlots[slot]) {
\t\t\tsObjectSlots[slot] = p2;
\t\t\tbreak;
\t\t}
\t\tif (++probe == kSlots) OSPanic(__FILE__, __LINE__, "Sequence object table full");
\t}
\tu32 handle = slot + 1;
\ttrack->writeRegDirect(p3, handle >> 16);
\ttrack->writeRegDirect(p3 + 1, handle & 0xFFFF);""")
    replace("src/plugProjectHikinoU/PSSystemIF.cpp", "u32 getObject(JASTrack* track, u8 p2)\n{",
            "static void* sObjectSlots[1 << 16];\n\nuintptr_t getObject(JASTrack* track, u8 p2)\n{")
    replace("src/plugProjectHikinoU/PSSystemIF.cpp",
            "\treturn ((hi << 16) & 0xFFFF0000 | (lo) & 0x0000FFFF);",
            "\tu32 handle = (hi << 16) | lo;\n\treturn handle ? uintptr_t(sObjectSlots[handle - 1]) : 0;")
    replace("include/PSSystem/PSSystemIF.h", "u32 getObject(JASTrack* track, u8 p2);", "uintptr_t getObject(JASTrack* track, u8 p2);")
    replace("src/plugProjectHikinoU/PSAutoBgm.cpp", "(PSBankData*)(offs + (u32)track->mSeqCtrl.mRawFilePtr);", "(PSBankData*)(offs + (uintptr_t)track->mSeqCtrl.mRawFilePtr);", count=2)
    replace("src/plugProjectHikinoU/PSAutoBgm.cpp", "(PSBankData*)((u32)mBankData + (u32)track->getSeq()->mRawFilePtr);", "(PSBankData*)((uintptr_t)mBankData + (uintptr_t)track->getSeq()->mRawFilePtr);")
    replace("src/plugProjectHikinoU/PSAutoBgm.cpp", "(PSWsData*)((u32)mWsData + (u32)track->getSeq()->mRawFilePtr);", "(PSWsData*)((uintptr_t)mWsData + (uintptr_t)track->getSeq()->mRawFilePtr);")
    replace("src/plugProjectHikinoU/PSAutoBgm.cpp", ", mWsDataNum(nullptr)", ", mWsDataNum(0)")
    replace("include/PSAutoBgm/PSAutoBgm.h", "static void loadedCallback(u32, u32);", "static void loadedCallback(u32, uintptr_t);")
    replace("src/plugProjectHikinoU/PSAutoBgm.cpp", "void AutoBgm::loadedCallback(u32 p1, u32 p2)", "void AutoBgm::loadedCallback(u32 p1, uintptr_t p2)")
    replace("src/plugProjectHikinoU/PSAutoBgm.cpp", "\tu32 ptr = (u32)this;", "\tuintptr_t ptr = (uintptr_t)this;")
    replace("src/plugProjectHikinoU/PSGame.cpp", "{ mSolidHeap, mHeapSize, 231, seqCpuSync, mAafFile,", "{ mSolidHeap, mHeapSize, 231, (void*)seqCpuSync, mAafFile,")
    replace("include/JSystem/JAudio/JAS/JASKernel.h", "if ((u32)mBuffer <= (u32)msg && (u32)msg < (u32)(this + 1)) {",
            "if ((uintptr_t)mBuffer <= (uintptr_t)msg && (uintptr_t)msg < (uintptr_t)(this + 1)) {")
    # The demo argument stores a small captain index in its pointer-typed field.
    replace("src/utilityU/PSMainSide_Demo.cpp", "switch ((u32)demoArg.mCameraName) {", "switch ((u32)(uintptr_t)demoArg.mCameraName) {")
    replace("src/utilityU/PSMainSide_ObjSound.cpp", "if ((int)data == 0xffffffff) {", "if ((u32)(uintptr_t)data == 0xffffffff) {")

    # Original off-by-one: the bottom-allocated path copy has no room for its
    # terminator. Console heap rounding usually hid it; natively it can fault.
    replace("src/sysGCU/loadResource.cpp", "path = new (arg.mHeap, -1) char[strlen(arg.mPath)];",
            "path = new (arg.mHeap, -1) char[strlen(arg.mPath) + 1];")
    # The console's DVD thread could not run until init() returned; here it starts
    # at once and read theTekiHeap (and set mMainHeap) before init assigned them.
    result = "src/plugProjectKandoU/singleGS_MainResult.cpp"
    replace(result, "\tsys->dvdLoadUseCallBack(&mDvdThread, mLoadDelegate);\n\tmStatus               = Result_LoadData;", "\tmStatus               = Result_LoadData;")
    replace(result, "\tmPelletMgr = gameSystem->detachObjectMgr_reuse(pelletMgr);\n}",
            "\tmPelletMgr = gameSystem->detachObjectMgr_reuse(pelletMgr);\n\tsys->dvdLoadUseCallBack(&mDvdThread, mLoadDelegate);\n}")
    # Original out-of-bounds read: at i == 0 these read list[-1]. On the console
    # that word was a harmless neighbouring pointer; natively it is text.
    dayend = "src/plugProjectKonoU/khDayEndResult.cpp"
    for counters in ("mPikiCountersList", "mDeathCountersList"):
        replace(dayend, f"\t\tif ({counters}[i - 1]->mSlotFinished && !{counters}[i]->_A9) {{",
                f"\t\tif (i > 0 && {counters}[i - 1]->mSlotFinished && !{counters}[i]->_A9) {{")
    # mail_table.bin is big-endian disc data read in place: the entry count and
    # each 64-bit message ID need swapping (flags and names are bytes).
    replace("include/kh/khDayEndResult.h", "\tinline u64 getMessageID() const { return mMessageID; }",
            "\tinline u64 getMessageID() const { return p2_read_big<u64>(reinterpret_cast<const u8*>(&mMessageID)); }")
    replace("include/kh/khDayEndResult.h", "struct MailTableDataEntry {", "#include \"p2_endian.h\"\nstruct MailTableDataEntry {")
    replace(dayend, "\tu32 entries = file->mEntries;",
            "\tif (!file) OSPanic(__FILE__, __LINE__, \"mail_table.bin missing: node %p archive %p mounted %d files %u\", tableNode,\n"
            "\t                   tableNode ? (void*)tableNode->mArchive : nullptr, tableNode && tableNode->mArchive ? tableNode->mArchive->isMounted() : -1,\n"
            "\t                   tableNode && tableNode->mArchive ? tableNode->mArchive->countFile() : 0u);\n"
            "\tu32 entries = p2_read_big<u32>(reinterpret_cast<const u8*>(&file->mEntries));")
    # The cave number is the ID's last character ('t_01' -> '1'): byte 3 on the
    # big-endian console, the low byte of the value here (byte 3 read 't' = cave 67).
    replace("src/plugProjectHikinoU/PSGame.cpp", "\treturn (u8)(mCaveID.byteView[3] - '1');",
            "\treturn (u8)((mCaveID.fullView & 0xFF) - '1');")
    # GX display lists stay big-endian (Aurora consumes them as such): the farm
    # vertex-colour setup read their position/colour indices and counts natively,
    # and the swapped indices wrote far past mInfo onto the model's packets.
    file = destination / "src/plugProjectYamashitaU/vtxAnm.cpp"
    text, count = re.subn(r"\*reinterpret_cast<u16\*>\(([^()]*(?:\([^()]*\))?[^()]*)\)", r"p2_read_big<u16>(\1)", file.read_text())
    if count != 19: raise RuntimeError(f"Vertex colour display-list patch drift: {count}")
    file.write_text('#include "p2_endian.h"\n' + text)
    # Name the exhausted heap by its size and parent, so an undersized native
    # heap shows which constant to raise.
    replace("src/sysGCU/system.cpp", "static void Pikmin2DefaultMemoryErrorRoutine(void* address, u32 size, int alignment)\n{",
            "static void Pikmin2DefaultMemoryErrorRoutine(void* address, u32 size, int alignment)\n{\n"
            "\tJKRHeap* full = static_cast<JKRHeap*>(address);\n"
            "\tp2_heap_reportf(\"*** heap %p of size 0x%x is full (parent %p, size 0x%x)\\n\", full, full->getHeapSize(), full->getParent(),\n"
            "\t                full->getParent() ? full->getParent()->getHeapSize() : 0u);")
    # Original double free on a failed load: ~Node() already deletes mName (path).
    replace("src/sysGCU/loadResource.cpp", "\t\t\tdelete node;\n\t\t\tdelete path;",
            "\t\t\tif (node) delete node; // owns path through mName\n\t\t\telse delete path;")

    # Missing returns. MWCC left the last call's result in r3, so the original
    # binary returned it; under Clang, falling off the end is undefined and the
    # CMake flags make it trap. Fix each reached site with the value MWCC returned.
    replace("src/sysGCU/titleSection.cpp",
            "\tgPikmin2AramMgr->setLoadPermission(false);\n\tsys->dvdLoadSyncNoBlock(&mThreadCommand);\n}",
            "\tgPikmin2AramMgr->setLoadPermission(false);\n\treturn sys->dvdLoadSyncNoBlock(&mThreadCommand);\n}")
    replace("src/sysGCU/system.cpp", "\tJUTGamePad::read();\n\tmDvdStatus->update();\n}",
            "\tJUTGamePad::read();\n\treturn mDvdStatus->update();\n}")

    # Scene objects hold pointer arrays (e.g. SceneBase's object table); 8-byte
    # native pointers overflow the console's 0x880-byte scene heap.
    replace("src/sysGCU/screenMgr.cpp", "JKRSolidHeap::create(0x880, JKRGetCurrentHeap(), true);",
            "JKRSolidHeap::create(0x880 * 2, JKRGetCurrentHeap(), true);")

    # Particle function tables are pointer arrays sized with the console's
    # 4-byte pointer; native tables overflowed into their neighbours.
    replace("src/JSystem/JParticle/JPAResource.cpp", "FuncListNum * 4, 4)",
            "FuncListNum * sizeof(void*), sizeof(void*))", count=7)

    # Native objects are larger than the console's (8-byte pointers, wider
    # vtables), so heaps sized for 32-bit layouts overflow (e.g. the High Scores
    # screen in the 2D resource heap). The native arena is 128 MiB, not 24 MiB.
    # The generator cache is bounded by the save-data format, so it stays.
    file = destination / "include/BuildSettings.h"
    text, scaled = re.subn(r"(?m)^(#define (?!GENERATOR_CACHE)\w*HEAP_SIZE\w*\s+)\((0x[0-9A-Fa-f]+)\)",
                           r"\1(2 * \2)", file.read_text())
    if scaled != 13: raise RuntimeError(f"Heap size patch drift: scaled {scaled} of 13 heaps")
    # The enemy Piklopedia index (one picture pane per enemy, native J2D
    # materials) still filled a doubled 2D heap: give it 4x, carved from a
    # system heap grown by the same amount.
    for old_size, new_size in (("(2 * 0xD4800) ", "(4 * 0xD4800) "), ("(2 * 0x428000)", "(2 * 0x428000 + 2 * 0xD4800)")):
        if text.count(old_size) != 1: raise RuntimeError("2D heap size patch drift: " + old_size)
        text = text.replace(old_size, new_size)
    file.write_text(text)

    # Message IDs are ASCII tags packed big-endian into a u64 ("8321_00");
    # reading the u64's memory as chars reverses them on little-endian hosts.
    replace("src/sysGCU/message.cpp", "\tinID = inID << 8;\n\tconvertCharToMessageID((char*)(&inID), messageID, variantID);",
            "\tinID = inID << 8;\n\tchar text[8];\n\tfor (int i = 0; i < 8; i++) {\n\t\ttext[i] = (char)(inID >> (56 - 8 * i));\n\t}\n\tconvertCharToMessageID(text, messageID, variantID);")

    def append_return(path, signature, expression):
        """Add `return expression;` before the closing brace of the function at `signature`."""
        file = destination / path
        text = file.read_text()
        if text.count(signature) != 1:
            raise RuntimeError(f"Patch drift in {path}: expected one {signature!r}")
        start = text.index("{", text.index(signature))
        depth = 0
        for match in re.finditer(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\\n])*'|[{}]", text[start:], re.S):
            depth += {"{": 1, "}": -1}.get(match[0], 0)
            if depth == 0:
                end = start + match.start()
                break
        file.write_text(text[:end] + f"\treturn {expression};\n" + text[end:])

    # Values MWCC returned by leaving a call's result in r3/f1.
    replace("src/sysGCU/messageMgr.cpp", "\tnew Mgr(heap);\n}", "\treturn new Mgr(heap);\n}")
    replace("src/JSystem/J2D/J2DTextBoxEx.cpp", "\tJ2DPane::animationPane(anm);\n}", "\treturn J2DPane::animationPane(anm);\n}")
    replace("src/sysGCU/messageRendering.cpp", "\tmMainFont->drawChar_scale(p1, p2, p3, p4, p5, p6);\n}",
            "\treturn mMainFont->drawChar_scale(p1, p2, p3, p4, p5, p6);\n}")
    replace("src/sysGCU/messageRendering.cpp", "\tmRubyFont->drawChar_scale(p1, p2, p3, p4, p5, p6);\n}",
            "\treturn mRubyFont->drawChar_scale(p1, p2, p3, p4, p5, p6);\n}")
    replace("src/plugProjectKonoU/newGame2DMgr.cpp",
            "\tSetSceneArg arg(SCENE_COURSE_NAME, &disp);\n\targ._08 = 1;\n\tmScreenMgr->setScene(arg);\n\tmScreenMgr->startScene(nullptr);\n}",
            "\tSetSceneArg arg(SCENE_COURSE_NAME, &disp);\n\targ._08 = 1;\n\tmScreenMgr->setScene(arg);\n\treturn mScreenMgr->startScene(nullptr);\n}")
    replace("src/JSystem/JMessage/processor.cpp", "\t\tTProcessor::do_tag_(p1, p2, p3);\n\t}\n}",
            "\t\treturn TProcessor::do_tag_(p1, p2, p3);\n\t}\n\treturn true;\n}")
    replace("src/sysGCU/illustratedBookMessage.cpp", "\tP2JME::TControl::update();\n\tmMaxScroll",
            "\tconst bool result = P2JME::TControl::update();\n\tmMaxScroll")
    append_return("src/sysGCU/illustratedBookMessage.cpp", "bool TControl::update(Controller* control1, Controller* control2)", "result")
    # Gameplay sites. Calls whose result MWCC left in r3/f1:
    for path, old, new in (
            ("include/Game/Entities/KingChappy.h", "\t\tEnemyBase::eatWhitePikminCallBack(creature, C_PROPERPARMS.mWhitePikmin.mValue);\n\t}",
             "\t\treturn EnemyBase::eatWhitePikminCallBack(creature, C_PROPERPARMS.mWhitePikmin.mValue);\n\t}"),
            ("src/plugProjectHikinoU/PSSe.cpp", "\tobj->startSound(randomID, flag);\n}", "\treturn reinterpret_cast<JAISe*>(obj->startSound(randomID, flag));\n}"),
            ("src/plugProjectHikinoU/PSSeBase.cpp", "\tobj->startSound(mInitArg.mSoundID, 0);\n}", "\treturn obj->startSound(mInitArg.mSoundID, 0);\n}"),
            ("src/plugProjectHikinoU/PSSystemIF.cpp", "\t\tsMakeJAISeCallback();\n\t} else {", "\t\treturn reinterpret_cast<JAISe*>(sMakeJAISeCallback());\n\t} else {"),
            ("src/plugProjectKandoU/gameSystem.cpp", "\tstartPause(isPausedSoft, pauseID, str);\n}", "\treturn startPause(isPausedSoft, pauseID, str);\n}"),
            ("src/plugProjectKandoU/genItem.cpp", "\tbirth(&arg);\n}", "\treturn birth(&arg);\n}"),
            ("src/plugProjectMorimuraU/dayEndCount.cpp", "\tmOffsetY = -m2pOffsetY;\n\tTDayEndCount::doUpdate();\n}", "\tmOffsetY = -m2pOffsetY;\n\treturn TDayEndCount::doUpdate();\n}"),
            ("src/plugProjectMorimuraU/dayEndCount.cpp", "\tmOffsetY = m2pOffsetY;\n\tTDayEndCount::doUpdate();\n}", "\tmOffsetY = m2pOffsetY;\n\treturn TDayEndCount::doUpdate();\n}"),
            ("src/plugProjectMorimuraU/panModoki.cpp", "\tpressCallBack(source, damage, part);\n}", "\treturn pressCallBack(source, damage, part);\n}"),
            ("src/plugProjectNishimuraU/KumaKochappy.cpp", "\tpressCallBack(creature, damage, part);\n}", "\treturn pressCallBack(creature, damage, part);\n}"),
            ("src/plugProjectNishimuraU/Sarai.cpp", "\tEnemyFunc::eatPikmin(this, nullptr);\n}", "\treturn EnemyFunc::eatPikmin(this, nullptr);\n}"),
            ("src/plugProjectOgawaU/ogObjSMenuMap.cpp", "\tstopYaji();\n\tstart_LR(arg);\n}", "\tstopYaji();\n\treturn start_LR(arg);\n}"),
            ("src/plugProjectOgawaU/ogObjSMenuPause.cpp", "\tstopYaji();\n\tstart_LR(arg);\n}", "\tstopYaji();\n\treturn start_LR(arg);\n}"),
            ("src/plugProjectOgawaU/ogObjSMenuPauseDoukutu.cpp", "\tstopYaji();\n\tstart_LR(arg);\n}", "\tstopYaji();\n\treturn start_LR(arg);\n}"),
            ("src/plugProjectOgawaU/ogObjSMenuMap.cpp", "\tcommonUpdate();\n\tupdateFadeOut();\n}", "\tcommonUpdate();\n\treturn updateFadeOut();\n}"),
            ("src/utilityU/PSMainSide_Sound.cpp", "\tPSSystem::getSoundCategoryInfo(mgr, soundCat)->getDistVol(p1, p2);\n}",
             "\treturn PSSystem::getSoundCategoryInfo(mgr, soundCat)->getDistVol(p1, p2);\n}")):
        replace(path, old, new)
    # Fall-through after a false check (MWCC's r3 held that false result),
    # unreachable tails after a panic, and empty/unused bodies.
    for path, signature, value in (
            ("include/Game/VsGame.h", "inline bool getMarbleLoss(bool& loseRed, bool& loseBlue)", "false"),
            ("src/plugProjectKandoU/gamePlayData.cpp", "int PlayData::getTekiCarcassMoney(int id)", "0"),
            ("src/plugProjectKandoU/gamePlayData.cpp", "bool PlayData::isCaveFirstReturn(int, ID32&)", "false"),
            ("src/plugProjectKandoU/gamePlayData.cpp", "bool PlayData::closeCourse(int)", "false"),
            ("src/plugProjectKandoU/gamePlayData.cpp", "bool PlayData::doneWorldMapEffect()", "true"),
            ("src/plugProjectKandoU/gamePlayData.cpp", "bool PlayData::isPelletEverGot(Pellet* pellet)", "false"),
            ("src/plugProjectKandoU/gamePlayData.cpp", "bool PlayData::isPelletEverGot(u8 type, u8 id)", "false"),
            ("src/plugProjectKandoU/interactNavi.cpp", "bool InteractSarai::actNavi(Game::Navi* navi)", "false"),
            ("src/plugProjectKandoU/interactNavi.cpp", "bool InteractDenki::actNavi(Game::Navi* navi)", "false"),
            ("src/plugProjectKandoU/interactNavi.cpp", "bool InteractFlick::actNavi(Game::Navi* navi)", "false"),
            ("src/plugProjectKandoU/interactNavi.cpp", "bool InteractPress::actNavi(Game::Navi* navi)", "false"),
            ("src/plugProjectKandoU/interactNavi.cpp", "bool InteractBubble::actNavi(Game::Navi* navi)", "false"),
            ("src/plugProjectKandoU/itemMgr.cpp", "PlatAttacher* BaseItemMgr::loadPlatAttacher(JKRFileLoader* loader, char* path)", "nullptr"),
            ("src/plugProjectKandoU/singleGS_MainGame.cpp", "unknown GameState::gameStart(SingleGameSection*)", "0"),
            ("src/plugProjectKandoU/singleGS_MainResult.cpp", "unknown MainResultState::open2D(SingleGameSection* game)", "0"),
            ("src/plugProjectKandoU/vsCardMgr.cpp", "Vector3f VsGame::CardMgr::getPlayerCard(int user)", "Vector3f(0.0f, 0.0f, 0.0f)"),
            ("src/plugProjectKandoU/vsCardMgr.cpp", "bool CardMgr::SlotMachine::goodPlace()", "false"),
            ("src/plugProjectKandoU/vsGameSection.cpp", "bool GameMessageVsBirthTekiTreasure::actVs(VsGameSection* section)", "false"),
            ("src/plugProjectKandoU/vsGameSection.cpp", "bool VsGameSection::sendMessage(GameMessage& message)", "false"),
            ("src/plugProjectMorimuraU/dayEndCount.cpp", "bool TCountDownScene::doStart(Screen::StartSceneArg* arg)", "false"),
            ("src/plugProjectOgawaU/ogObjSMenuMap.cpp", "u8 ObjSMenuMap::calcCaveNameAlpha()", "alpha"),
            ("src/plugProjectOgawaU/ogObjVs.cpp", "bool ObjVs::startGetBdama(J2DPane* pane)", "false"),
            ("src/sysCommonU/sysMath.cpp", "Vector3f CRSpline(f32 t, Vector3f* controls)", "Vector3f(0.0f, 0.0f, 0.0f)")):
        append_return(path, signature, value)
    # MWCC's r3 held the false isCalc() result on the fall-through path.
    append_return("src/plugProjectEbisawaU/ebiP2Title.cpp", "bool TTitleMgr::inField(TObjBase* obj)", "false")
    # No caller uses these values (TP declares the JMessage ones void).
    for path, signature, value in (
            ("src/plugProjectEbisawaU/ebiP2Title.cpp", "Vector2f TTitleMgr::setStartPosToPiki()", "Vector2f(0.0f, 0.0f)"),
            ("src/plugProjectEbisawaU/ebiP2Title.cpp", "bool TTitleMgr::inViewField(Vector2f& pos, f32 radius)", "true"),
            ("src/plugProjectEbisawaU/ebiP2Title.cpp", "bool TTitleMgr::update()", "true"),
            ("src/JSystem/JMessage/processor.cpp", "unknown TProcessor::on_select_begin(", "0"),
            ("src/JSystem/JMessage/processor.cpp", "bool TProcessor::do_tag_(u32 tag, const void* data, u32 size)", "true"),
            ("src/JSystem/JMessage/processor.cpp", "bool TSequenceProcessor::do_tag_(u32 tag, const void* data, u32 size)", "true"),
            ("src/sysCommonU/stream.cpp", "char* Stream::readFixedString()", "nullptr"),
            ("src/sysGCU/graphics.cpp", "SysShape::Model* Viewport::setJ3DViewMtx(bool flag)", "nullptr")):
        append_return(path, signature, value)
    for path in (project / "tools/switch_scope_files.txt").read_text().splitlines():
        file = destination / path
        file.write_text(scope_switches(file.read_text()))
    # Metrowerks accepts 5-8 byte character literals as 64-bit pane tags.
    # Clang truncates these to int. Skip comments and strings while translating.
    token = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\\n])*'", re.S)
    def native_tag(match):
        literal = match[0]
        if not literal.startswith("'"):
            return literal
        import ast
        value = ast.literal_eval(literal).encode("latin1")
        if len(value) <= 4:
            return literal
        if len(value) > 8:
            raise RuntimeError(f"Oversized character tag: {literal}")
        return f"0x{int.from_bytes(value, 'big'):016x}ULL"
    for file in destination.rglob("*"):
        if file.suffix in (".h", ".cpp", ".c"):
            original = file.read_text()
            converted = token.sub(native_tag, original)
            if converted != original:
                file.write_text(converted)
    jaudio_patches.apply(destination, replace, sub_exact, native_body)
    # Return-handle starts mark an SE request with the sentinel (T*)1 and release what comes back. With the
    # silent sound system there is no sound table, getInfoPointer is null and the sentinel survives.
    replace("include/JSystem/JAudio/JAI/JAIBasic.h", "\t*handlePtr = *tempHandle;\n\tif (*tempHandle) {",
            "\tif (*tempHandle == (T*)(1))\n\t\t*tempHandle = nullptr; // no sound started\n\t*handlePtr = *tempHandle;\n\tif (*tempHandle) {", count=2)
    # Input record/replay (src/input_record.cpp): the PADRead call is the tick, and the boot seed is pinned.
    replace('src/JSystem/JUtility/JUTGamePad.cpp', 'sRumbleSupported = PADRead(mPadStatus);',
            'sRumbleSupported = PADRead(mPadStatus);\n\tp2_input_tick(mPadStatus);', count=1)
    replace('src/JSystem/JUtility/JUTGamePad.cpp', '#include "JSystem/JUtility/JUTGamePad.h"',
            '#include "JSystem/JUtility/JUTGamePad.h"\nextern "C" void p2_input_tick(void* statuses);', count=1)
    replace('src/sysGCU/system.cpp', 'p2_game_srand(OSGetTick());', 'p2_game_srand(p2_input_boot_seed(OSGetTick()));', count=1)
    replace('src/sysGCU/system.cpp', '#include "System.h"', '#include "System.h"\nextern "C" u32 p2_input_boot_seed(u32 natural);', count=1)
    # Reads of uninitialized locals (-Wuninitialized, -Wsometimes-uninitialized).
    # Real bugs: a self-initialised pointer, and an asm-only reciprocal estimate.
    replace("src/plugProjectNishimuraU/ArmorState.cpp", "Obj* armor = OBJ(armor);", "Obj* armor = OBJ(enemy);")
    replace("include/JSystem/JMath.h", "\tregister f32 recip;\n\n\tif (x > 0.0f) {", "\tregister f32 recip = (f32)__frsqrte(x);\n\n\tif (x > 0.0f) {")
    # The original left a stale r29 in sound register 3 for these four commands; keep the register's value instead.
    replace("src/JSystem/JAudio/JAS/JASTrack.cpp", "\tu16 val29;\n\tswitch (nextByte) {", "\tu16 val29 = mRegisterParam._00[3];\n\tswitch (nextByte) {")
    # Paths the shipped data or callers never take (unknown enum, release-build
    # panics that return, an unused number base): defined values instead of garbage.
    replace("src/JSystem/J2D/J2DPrint.cpp", "\tchar* endStr;\n\n\tif (base == 10)", "\tchar* endStr = (char*)*strPtr;\n\n\tif (base == 10)", count=2)
    replace("src/JSystem/JAudio/JAI/JAISequenceMgr.cpp", "\tJAISequence* seq;\n\tu32 playSeqNo = soundInfo->_05;", "\tJAISequence* seq = nullptr;\n\tu32 playSeqNo = soundInfo->_05;")
    replace("src/plugProjectKonoU/newGame2DMgr.cpp", "\tbool set;\n\n\tswitch (disp.mOpenMode) {", "\tbool set = false;\n\n\tswitch (disp.mOpenMode) {")
    replace("src/plugProjectMorimuraU/miulinState.cpp", "\t\t\tf32 x;\n\t\t\tf32 z;\n\t\t\tCreature* creature = enemy->mTargetCreature;",
            "\t\t\tf32 x = enemy->getPosition().x;\n\t\t\tf32 z = enemy->getPosition().z;\n\t\t\tCreature* creature = enemy->mTargetCreature;")
    replace("src/plugProjectYamashitaU/pelplant.cpp", "\t\tf32 neckScale;\n\t\tswitch (mSize) {", "\t\tf32 neckScale = 12.0f;\n\t\tswitch (mSize) {")
    replace("src/plugProjectYamashitaU/singleGS_ZukanParms.cpp", "\tint start, middle, stop;", "\tint start = SUNTIME_Noon, middle = SUNTIME_Noon, stop = SUNTIME_Noon;")
    replace("src/sysGCU/JSTFindCreature.cpp", "\t\tOnyonTypes onyon_type;", "\t\tOnyonTypes onyon_type = ONYON_TYPE_RED;")
    replace("src/sysGCU/dvdStatus.cpp", "\t\tchar** errorMsgSet;", "\t\tchar** errorMsgSet = DvdError::gMessage_eng;")
    replace("src/sysGCU/messageRendering.cpp", "\tint type;\n\tu8 firstByte = ((u8*)p2)[0]; // r29\n\tf32 width;\n\tf32 height;",
            "\tint type = 0;\n\tu8 firstByte = ((u8*)p2)[0]; // r29\n\tf32 width = 0.0f;\n\tf32 height = 0.0f;")
    replace("src/sysGCU/pikmin2MemoryCardMgr.cpp", "u32 Mgr::getCardStatus()\n{\n\tu32 result;", "u32 Mgr::getCardStatus()\n{\n\tu32 result = MCS_Ready;")
    replace("include/Title.h", "\t\tint unused; // no clue", "\t\tint unused = 0; // no clue")
    # Console-only stubs that called through uninitialized pointers to emit weak functions.
    replace("src/JSystem/J2D/J2DBloSaver.cpp", "\tJ2DTevBlock* block;\n\tblock->setTexNo(0, 0);\n\tu16 texNo = block->getTexNo(0);\n", "")
    replace("src/JSystem/J2D/J2DBloSaver.cpp", "\tJ2DTevBlock* block;\n\tblock->getFontNo();\n\tblock->getTevOrder(0);\n\tblock->getTevSwapModeTable(0);\n", "")
    replace("src/JSystem/J2D/J2DBloSaver.cpp", "\tJ2DIndBlock* block;\n\tblock->getIndTexStageNum();\n", "")
    replace("src/sysGCU/wipe.cpp", "\tWipeBase* base;\n\tbase->isBlack();\n\tbase->isWhite();\n", "")
    # -Wconditional-uninitialized: locals set on every path the shipped data or
    # callers take, read on paths they never take. A defined value replaces
    # garbage there (and keeps replays deterministic); taken paths are unchanged.
    def init_declaration(path, old, new, count):
        file = destination / path
        text = file.read_text()
        pattern = re.compile(r"^([ \t]*)" + re.escape(old), re.M)
        text, found = pattern.subn(lambda m: m[1] + new, text)
        if found != count:
            raise RuntimeError(f"Patch drift in {path}: expected {count} declarations {old!r}, found {found}")
        file.write_text(text)
    for path, old, new, count in (
            ("src/JSystem/J2D/J2DScreen.cpp", "u32 x1, y1, wd, ht;", "u32 x1 = 0, y1 = 0, wd = 0, ht = 0;", 1),
            ("src/JSystem/J2D/J2DWindowEx.cpp", "f32 in_f31;", "f32 in_f31 = 0.0f;", 1),
            ("src/JSystem/J2D/J2DWindowEx.cpp", "f32 in_f30;", "f32 in_f30 = 0.0f;", 1),
            ("src/JSystem/J2D/J2DWindowEx.cpp", "f32 in_f29;", "f32 in_f29 = 0.0f;", 1),
            ("src/JSystem/J2D/J2DWindowEx.cpp", "f32 in_f28;", "f32 in_f28 = 0.0f;", 1),
            ("src/JSystem/JAudio/JAI/JAISoundTable.cpp", "u32 category;", "u32 category = 0;", 1),
            ("src/JSystem/JAudio/JAS/JASAramStream.cpp", "u8 blockType;", "u8 blockType = 0;", 1),
            ("src/JSystem/JAudio/JAS/JASChannel.cpp", "f32 pan;", "f32 pan = 0.0f;", 1),
            ("src/JSystem/JAudio/JAS/JASChannel.cpp", "f32 fxmix;", "f32 fxmix = 0.0f;", 1),
            ("src/JSystem/JAudio/JAS/JASChannel.cpp", "f32 scale;", "f32 scale = 0.0f;", 1),
            ("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", "void* base;", "void* base = nullptr;", 1),
            ("src/JSystem/JAudio/JAS/JASSeqParser.cpp", "u32 data;", "u32 data = 0;", 2),
            ("src/JSystem/JAudio/JAS/JASSeqParser.cpp", "s16 data;", "s16 data = 0;", 1),
            ("src/JSystem/JAudio/JAS/JASSeqParser.cpp", "u8 val27;", "u8 val27 = 0;", 1),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "f32 vol;", "f32 vol = 0.0f;", 1),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "f32 pan;", "f32 pan = 0.0f;", 1),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "f32 fxmix;", "f32 fxmix = 0.0f;", 1),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "f32 dolby;", "f32 dolby = 0.0f;", 1),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "u32 result;", "u32 result = 0;", 2),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "u32 val28;", "u32 val28 = 0;", 1),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "u32 val25;", "u32 val25 = 0;", 1),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "u32 val24;", "u32 val24 = 0;", 1),
            ("src/JSystem/JAudio/JAS/JASTrack.cpp", "s16 val23;", "s16 val23 = 0;", 1),
            ("src/JSystem/JKernel/JKRDecomp.cpp", "u32 chunkBits;", "u32 chunkBits = 0;", 1),
            ("src/JSystem/JKernel/JKRDecomp.cpp", "s32 chunkBits;", "s32 chunkBits = 0;", 1),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", "u32 usedSize;", "u32 usedSize = 0;", 1),
            ("src/plugProjectEbisawaU/ebiMainTitleMgr.cpp", "int id;", "int id = 0;", 1),
            ("src/plugProjectEbisawaU/ebiScreenFileSelect.cpp", "int id1, id2;", "int id1 = 0, id2 = 0;", 1),
            ("src/plugProjectEbisawaU/ebiScreenFileSelect_Mgr.cpp", "bool save;", "bool save = false;", 1),
            ("src/plugProjectEbisawaU/ebiScreenOmakeGame.cpp", "u64 tag;", "u64 tag = 0;", 1),
            ("src/plugProjectEbisawaU/ebiScreenSaveMenu.cpp", "og::Screen::AnimText_Screen* screen;", "og::Screen::AnimText_Screen* screen = nullptr;", 1),
            ("src/plugProjectEbisawaU/ebiScreenTitleMenu.cpp", "E2DCallBack_AnmBase* anm;", "E2DCallBack_AnmBase* anm = nullptr;", 1),
            ("src/plugProjectKandoU/gameMapParts.cpp", "Sys::TriIndexList* list;", "Sys::TriIndexList* list = nullptr;", 1),
            ("src/plugProjectKandoU/itemRock.cpp", "int type;", "int type = 0;", 1),
            ("src/plugProjectKandoU/mapMgrTraceMove.cpp", "Sys::TriIndexList* triList;", "Sys::TriIndexList* triList = nullptr;", 1),
            ("src/plugProjectKandoU/naviState.cpp", "f32 nearestDistance;", "f32 nearestDistance = 0.0f;", 1),
            ("src/plugProjectKandoU/naviState.cpp", "Piki* newPiki;", "Piki* newPiki = nullptr;", 1),
            ("src/plugProjectKandoU/onyonMgr.cpp", "int objectType;", "int objectType = 0;", 1),
            ("src/plugProjectKandoU/onyonMgr.cpp", "int objType;", "int objType = 0;", 1),
            ("src/plugProjectMorimuraU/blackMan.cpp", "int targetWPIdx;", "int targetWPIdx = 0;", 1),
            ("src/plugProjectMorimuraU/hiScore2D.cpp", "f32 x, y;", "f32 x = 0.0f, y = 0.0f;", 1),
            ("src/plugProjectNishimuraU/RandItemUnit.cpp", "MapNode* dropNode;", "MapNode* dropNode = nullptr;", 1),
            ("src/plugProjectYamashitaU/enemyStoneDrawInfo.cpp", "f32 p2;", "f32 p2 = 0.0f;", 1),
            ("src/plugProjectYamashitaU/enemyStoneDrawInfo.cpp", "f32 p3;", "f32 p3 = 0.0f;", 1),
            ("src/plugProjectYamashitaU/generalEnemyMgr.cpp", "EnemyMgrBase* mgr;", "EnemyMgrBase* mgr = nullptr;", 1),
            ("src/plugProjectYamashitaU/pelplant.cpp", "f32 headScale;", "f32 headScale = 0.0f;", 1),
            ("src/plugProjectYamashitaU/pelplant.cpp", "f32 neckLength;", "f32 neckLength = 0.0f;", 1),
            ("src/plugProjectYamashitaU/pelplant.cpp", "f32 neckThickness;", "f32 neckThickness = 0.0f;", 1),
            ("src/sysGCU/loadResource.cpp", "Node* node;", "Node* node = nullptr;", 1),
            ("src/sysGCU/loadResource.cpp", "char* path;", "char* path = nullptr;", 1),
            ("src/sysGCU/pikmin2MemoryCardMgr.cpp", "bool result;", "bool result = false;", 4),
    ):
        init_declaration(path, old, new, count)
    # Arrays from new[] released with scalar delete (-Wmismatched-new-delete).
    replace("src/sysGCU/pikmin2MemoryCardMgr.cpp", "delete (buffer);", "delete[] buffer;", count=9)
    replace("src/sysGCU/pikmin2MemoryCardMgr.cpp", "delete (optionBuffer);", "delete[] optionBuffer;")
    replace("src/sysGCU/memoryCard.cpp", "delete (buffer);", "delete[] buffer;")
    replace("src/plugProjectKandoU/singleGS_Zukan.cpp", "\t\tdelete spawnPositions;\n\t\tdelete tempPositions;",
            "\t\tdelete[] spawnPositions;\n\t\tdelete[] tempPositions;")
    # Integers carried in pointer types (-Wint-to-pointer-cast): indices used as
    # iterator cookies, message values, ARAM addresses and data-relative offsets.
    # The widening is intended; the explicit cast says so.
    for path, old, new, count in (
            ("include/Container.h", 'return (void*)mCount;', 'return (void*)(intptr_t)mCount;', 1),
            ("include/Game/EnemyMgrBase.h", 'return (void*)mObjLimit; }', 'return (void*)(intptr_t)mObjLimit; }', 1),
            ("include/MonoObjectMgr.h", 'return (void*)mMax;', 'return (void*)(intptr_t)mMax;', 3),
            ("include/MonoObjectMgr.h", 'return (void*)i;', 'return (void*)(intptr_t)i;', 1),
            ("include/mapCode.h", 'return (char*)mContents; }', 'return (char*)(uintptr_t)mContents; }', 1),
            ("src/JSystem/JAudio/JAS/JASAramStream.cpp", '(void*)((u32)p1 << 0x10 | 1)', '(void*)(uintptr_t)((u32)p1 << 0x10 | 1)', 1),
            ("src/JSystem/JAudio/JAS/JASAramStream.cpp", 'OSSendMessage(&mMsgQueueA, (void*)msg,', 'OSSendMessage(&mMsgQueueA, (void*)(uintptr_t)msg,', 1),
            ("src/JSystem/JAudio/JAS/JASAramStream.cpp", '(void*)(mDataOffset + (i * (sBlockSize * mBlockCount)))', '(void*)(uintptr_t)(mDataOffset + (i * (sBlockSize * mBlockCount)))', 1),
            ("src/JSystem/JAudio/JAS/JASBankMgr.cpp", '= (void*)wavePtr;', '= (void*)(uintptr_t)wavePtr;', 1),
            ("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", '= (u8*)(aramBase + 31 & ~31);', '= (u8*)(uintptr_t)(aramBase + 31 & ~31);', 1),
            ("src/JSystem/JKernel/JKRAramStream.cpp", '(void*)writtenLength,', '(void*)(uintptr_t)writtenLength,', 1),
            ("src/JSystem/JKernel/JKRCompArchive.cpp", '(void*)(_64 + fileEntry->mDataOffset)', '(void*)(uintptr_t)(_64 + fileEntry->mDataOffset)', 1),
            ("src/JSystem/JKernel/JKRCompArchive.cpp", '(u8*)(_64 + fileEntry->mDataOffset)', '(u8*)(uintptr_t)(_64 + fileEntry->mDataOffset)', 1),
            ("src/JSystem/JUtility/JUTException.cpp", '(OSSectionInfo*)module->sectionInfoOffset', '(OSSectionInfo*)(uintptr_t)module->sectionInfoOffset', 1),
            ("src/JSystem/JUtility/JUTException.cpp", 'stack = (u32*)stack[0];', 'stack = (u32*)(uintptr_t)stack[0];', 1),
            ("src/JSystem/JUtility/JUTException.cpp", 'search_name_part((u8*)name_offset,', 'search_name_part((u8*)(uintptr_t)name_offset,', 1),
            ("src/JSystem/JUtility/JUTGamePad.cpp", '(void*)this->_50);', '(void*)(uintptr_t)this->_50);', 1),
            ("src/JSystem/JUtility/JUTVideo.cpp", '(void*)retraceCount,', '(void*)(uintptr_t)retraceCount,', 1),
            ("src/plugProjectHikinoU/PSAutoBgm.cpp", '(PSBankData*)(bnk[bnkVal]', '(PSBankData*)(uintptr_t)(bnk[bnkVal]', 1),
            ("src/plugProjectHikinoU/PSAutoBgm.cpp", '(PSWsData*)(ws[wsVal]', '(PSWsData*)(uintptr_t)(ws[wsVal]', 1),
            ("src/plugProjectHikinoU/PSSeq.cpp", 'heap->mOwner  = (SeqHeap*)arg1;', 'heap->mOwner  = (SeqHeap*)(uintptr_t)arg1;', 1),
            ("src/plugProjectKandoU/creatureStick.cpp", 'return (void*)numBuffer;', 'return (void*)(intptr_t)numBuffer;', 1),
            ("src/plugProjectKandoU/gameCPlate.cpp", 'return (void*)mSlotCount;', 'return (void*)(intptr_t)mSlotCount;', 1),
            ("src/plugProjectKandoU/pelletMgr.cpp", 'getObjectPtr((void*)mIndex)', 'getObjectPtr((void*)(intptr_t)mIndex)', 1),
            ("src/plugProjectKandoU/pelletMgr.cpp", 'getNext((void*)mIndex)', 'getNext((void*)(intptr_t)mIndex)', 1),
            ("src/plugProjectKandoU/routeMgr.cpp", 'return (void*)mCount;', 'return (void*)(intptr_t)mCount;', 1),
            ("src/plugProjectKandoU/singleGS_DayEnd.cpp", 'arg.mPelletName    = (char*)id;', 'arg.mPelletName    = (char*)(intptr_t)id;', 1),
            ("src/plugProjectYamashitaU/enemyMgrBase.cpp", 'return (void*)i;', 'return (void*)(intptr_t)i;', 1),
            ("src/plugProjectYamashitaU/enemyMgrBase.cpp", 'return (void*)mObjLimit;', 'return (void*)(intptr_t)mObjLimit;', 1),
    ):
        replace(path, old, new, count=count)
    # A debug string whose result is discarded; its %d argument was a char*.
    replace("src/plugProjectKandoU/aiPrimitives.cpp",
            '\t\tsprintf(buf, "%d->%d->...->%d", mStartNode->mWpIndex, (mStartNode->mNext) ? (char*)mStartNode->mNext->mWpIndex : "...", endIdx);\n', "")
    # Jumps through sequence registers 0x28-0x2B: 32-bit values that the console
    # used as absolute addresses. No host pointer fits, so stop by name.
    replace("src/JSystem/JAudio/JAS/JASSeqParser.cpp", "\t\t\tctrl->start((void*)trackptr, 0);",
            '\t\t\tOSPanic(__FILE__, __LINE__, "Sequence jump through 32-bit register pointer %08x", trackptr);')
    # _16 is u16[2]; the decomp's third element is the next field (-Warray-bounds).
    for old, new in (("\t_16[2] = 0;", "\t_1A = 0;"), ("\t_16[2] = 0x40;", "\t_1A = 0x40;"), ("\t_16[2] = other._16[2];", "\t_1A = other._1A;")):
        replace("src/JSystem/JAudio/JAS/JASRegisterParam.cpp", old, new)
    # printf-style game functions: let -Wformat check every call's arguments.
    for path, decl, fmt, first in (
            ("include/stream.h", 'void printf(char*, ...);', 2, 3),
            ("include/stream.h", 'void textWriteText(char*, ...);', 2, 3),
            ("include/Graphics.h", 'void perspPrintf(PerspPrintfInfo& info, Vector3f& position, char* format, ...);', 4, 5),
            ("include/Dolphin/db.h", 'void DBPrintf(const char* format, ...);', 1, 2),
            ("include/Dolphin/os.h", 'void OSReport(const char* message, ...);', 1, 2),
            ("include/Dolphin/os.h", 'void OSPanic(const char* file, int line, const char* message, ...);', 3, 4),
            ("include/JSystem/JAudio/JAS/JASReport.h", 'void JASReport(const char* str, ...);', 1, 2),
            ("include/JSystem/J2D/J2DPrint.h", 'f32 print(f32 x, f32 y, const char* format, ...);', 4, 5),
            ("include/JSystem/J2D/J2DPrint.h", 'f32 print(f32 x, f32 y, u8 alpha, const char* format, ...);', 5, 6),
            ("include/JSystem/J2D/J2DPrint.h", 'f32 getWidth(const char* format, ...);', 2, 3),
            ("include/JSystem/J2D/J2DPrint.h", 'void print(const char*, ...);', 2, 3),
            ("include/JSystem/J2D/J2DPrint.h", 'void print(u8, const char*, ...);', 3, 4),
            ("include/JSystem/J2D/J2DPrint.h", 'void getSize(TSize&, const char*, ...);', 3, 4),
            ("include/JSystem/J2D/J2DPrint.h", 'void getHeight(const char*, ...);', 2, 3),
            ("include/JSystem/J2D/J2DTextBox.h", 'size_t setString(const char*, ...);', 2, 3),
            ("include/JSystem/J2D/J2DTextBox.h", 'void setString(s16, const char*, ...);', 3, 4),
            ("include/JSystem/JUtility/JUTAssertion.h", 'void showAssert_f(u32, const char*, int, const char*, ...);', 4, 5),
            ("include/JSystem/JUtility/JUTAssertion.h", 'void setWarningMessage_f(u32, char*, int, const char*, ...);', 4, 5),
            ("include/JSystem/JUtility/JUTAssertion.h", 'void setLogMessage_f(u32, char*, int, const char*, ...);', 4, 5),
            ("include/JSystem/JUtility/JUTException.h", 'static void panic_f(char const* file, int line, char const* msg, ...);', 3, 4),
            ("include/JSystem/JUtility/JUTConsole.h", 'void print_f(const char*, ...);', 2, 3),
            ("include/JSystem/JUtility/JUTConsole.h", 'void JUTReportConsole_f(const char*, ...);', 1, 2),
            ("include/JSystem/JUtility/JUTConsole.h", 'void JUTWarningConsole_f(const char*, ...);', 1, 2),
            ("include/JSystem/JUtility/JUTDirectPrint.h", 'void drawString_f(u16 x, u16 y, const char* format, ...);', 4, 5),
    ):
        replace(path, decl, decl[:-1] + f" __attribute__((format(printf, {fmt}, {first})));")
    replace("src/sysGCU/system.cpp", "static void kando_panic_f(bool r3, const char* file, int line, const char* format, ...)\n",
            "__attribute__((format(printf, 4, 5))) static void kando_panic_f(bool r3, const char* file, int line, const char* format, ...)\n")
    # -Wformat findings. Pointers printed with %x (truncated on 64-bit) become
    # %p; calls missing their arguments print the conversion literally instead
    # of reading garbage varargs; text used as a format goes through "%s".
    for path, old, new, count in (
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'(block = %x)", block', r'(block = %p)", block', 1),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'":::addr %08x: bad heap signature.', r'":::addr %p: bad heap signature.', 1),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'":::addr %08x: bad next pointer (%08x)', r'":::addr %p: bad next pointer (%p)', 1),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'":::addr %08x: bad previous pointer (%08x)', r'":::addr %p: bad previous pointer (%p)', 2),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'":::addr %08x: bad used list(REV) (%08x)', r'":::addr %p: bad used list(REV) (%p)', 2),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'":::addr %08x: bad block size (%08x)', r'":::addr %p: bad block size (%08x)', 1),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'"xxxxx %08x: --------  --- ---  (-------- --------)\nabort\n", block', r'"xxxxx %p: --------  --- ---  (-------- --------)\nabort\n", block', 1),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'p2_heap_reportf("xxxxx %08x: --------  --- ---  (-------- --------)\nabort\n");', r'p2_heap_reportf("xxxxx --------: --------  --- ---  (-------- --------)\nabort\n");', 1),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'"%s %08x: %08x  %3d %3d  (%08x %08x)\n"', r'"%s %p: %08x  %3d %3d  (%p %p)\n"', 4),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'p2_heap_reportf("| %08x  ");', r'p2_heap_reportf("| %%08x  ");', 1),
            ("src/JSystem/JKernel/JKRExpHeap.cpp", r'p2_heap_reportf("%2x  %3d  %6x  (%08x %08x)\n");', r'p2_heap_reportf("%%2x  %%3d  %%6x  (%%08x %%08x)\n");', 1),
            # Routine: the game deletes objects living in solid heaps, which cannot
            # free. The console sent this to the hidden debug console; stay quiet.
            ("src/JSystem/JKernel/JKRSolidHeap.cpp", '\tp2_heap_reportf("free: cannot free memory block (%08x)\\n", block);',
             '\t(void)block; // solid heaps release only all at once', 1),
            ("src/JSystem/JKernel/JKRSolidHeap.cpp", r'"resize: cannot resize memory block (%08x: %d)\n"', r'"resize: cannot resize memory block (%p: %d)\n"', 1),
            ("src/JSystem/JKernel/JKRSolidHeap.cpp", r'"getSize: cannot get memory block size (%08x)\n"', r'"getSize: cannot get memory block size (%p)\n"', 1),
            ("src/JSystem/JKernel/JKRSolidHeap.cpp", r'"head %08x: %08x\n"', r'"head %p: %08x\n"', 1),
            ("src/JSystem/JKernel/JKRSolidHeap.cpp", r'"tail %08x: %08x\n"', r'"tail %p: %08lx\n"', 1),
            ("src/JSystem/JUtility/JUTException.cpp", r'"%08X:  %08X    %08X\n", stack,', r'"%p:  %08X    %08X\n", stack,', 1),
            ("src/JSystem/JUtility/JUTException.cpp", r'"CONTEXT:%08XH  (%s EXCEPTION)\n"', r'"CONTEXT:%pH  (%s EXCEPTION)\n"', 1),
            ("src/JSystem/JUtility/JUTException.cpp", r'"CONTEXT:%08XH\n"', r'"CONTEXT:%pH\n"', 1),
            ("src/JSystem/JUtility/JUTException.cpp", r'FrameMemory:%XH\n", getFrameMemory()', r'FrameMemory:%pH\n", getFrameMemory()', 2),
            ("src/plugProjectKandoU/cellIterator.cpp", r'"x %f>%f"', r'"x %d>%d"', 1),
            ("src/plugProjectKandoU/cellIterator.cpp", r'"y %f>%f"', r'"y %d>%d"', 1),
            ("src/plugProjectKandoU/cellIterator.cpp", r'"xy %f %f\n%f %f\n"', r'"xy %d %d\n%d %d\n"', 1),
            ("src/plugProjectKandoU/gameMapParts.cpp", r'"%s : not found !\n", nullptr);', r'"%s : not found !\n", "(null)");', 1),
            ("src/plugProjectKandoU/genItem.cpp", r'"no baseItemMgr for %s\n", &id32);', r'"no baseItemMgr for %s\n", id32.getStr());', 1),
            ("src/plugProjectOgawaU/ogSceneSMenuMap.cpp", r'"DispMember ERR! (%s)\n", 0);', r'"DispMember ERR! (%d)\n", 0);', 2),
            ("src/plugProjectOgawaU/ogSceneCourseName.cpp", r'"DispMember ERR!\n", 0);', r'"DispMember ERR!\n");', 1),
            ("src/plugProjectOgawaU/ogSceneFloor.cpp", r'sprintf(format, __FILE__, buffer);', r'sprintf(buffer, "%s", format);', 1),
            ("src/sysGCU/pikmin2MemoryCardMgr.cpp", r'padding:%d \n", sizeof(PlayerInfo),', r'padding:%d \n", (int)sizeof(PlayerInfo),', 1),
            ("src/sysGCU/system.cpp", r'"Memory Alloc Error!\n%x (size %d)', r'"Memory Alloc Error!\n%p (size %d)', 1),
            ("src/plugProjectHikinoU/PSGame.cpp", r'JUT_PANICLINE(999, "%s_%02d_0.cnd");', r'JUT_PANICLINE(999, "%%s_%%02d_0.cnd");', 1),
            ("src/plugProjectHikinoU/PSGame.cpp", r'JUT_PANICLINE(999, "%s_%02d_%1d.cnd");', r'JUT_PANICLINE(999, "%%s_%%02d_%%1d.cnd");', 1),
            ("src/plugProjectHikinoU/PSGame.cpp", r'JUT_PANICLINE(289, cndName);', r'JUT_PANICLINE(289, "%s", cndName);', 1),
            ("src/plugProjectKandoU/baseGameSection.cpp", r'"allocation dameck!\n%d/%d"', r'"allocation dameck!\n%%d/%%d"', 1),
            ("src/plugProjectKandoU/gameGenerator.cpp", r'"# generatorMgr <%s>\r\n"', r'"# generatorMgr <%%s>\r\n"', 1),
            ("src/plugProjectKandoU/gameGenerator.cpp", r'"\t# %d generators\r\n"', r'"\t# %%d generators\r\n"', 1),
            ("src/plugProjectKandoU/gameResultTexMgr.cpp", r'"cannot retrieve %d-th child (real child count = %d)!\n"', r'"cannot retrieve %%d-th child (real child count = %%d)!\n"', 1),
            ("src/plugProjectKandoU/singleGS_Zukan.cpp", r'OSReport("radius:%6.3f");', r'OSReport("radius:%%6.3f");', 1),
            ("src/plugProjectKandoU/singleGS_Zukan.cpp", r'OSReport("angle :%6.3f");', r'OSReport("angle :%%6.3f");', 1),
            ("src/plugProjectKandoU/singleGS_Zukan.cpp", r'OSReport("height:%6.3f");', r'OSReport("height:%%6.3f");', 1),
            ("src/plugProjectKandoU/singleGS_Zukan.cpp", r'OSReport("fovy  :%6.3f");', r'OSReport("fovy  :%%6.3f");', 1),
            ("src/plugProjectKandoU/singleGS_Zukan.cpp", r'OSReport("READY:%d Enemy:%d Item:%d");', r'OSReport("READY:%%d Enemy:%%d Item:%%d");', 1),
            ("src/plugProjectKandoU/singleGS_Zukan.cpp", r'OSReport("enemy:%d item:%d");', r'OSReport("enemy:%%d item:%%d");', 1),
            ("src/plugProjectKandoU/singleGS_Zukan.cpp", r'OSReport("heapA %d");', r'OSReport("heapA %%d");', 1),
            ("src/plugProjectKandoU/singleGS_Zukan.cpp", r'OSReport("heapB %d");', r'OSReport("heapB %%d");', 1),
            ("src/sysGCU/JSTObjectSystem.cpp", r'"JSGFindObject---- %d not found\n"', r'"JSGFindObject---- %%d not found\n"', 1),
            ("src/sysGCU/moviePlayer.cpp", r'OSReport("use  %.1fK");', r'OSReport("use  %%.1fK");', 1),
            ("src/sysGCU/moviePlayer.cpp", r'OSReport("heap %.1fK");', r'OSReport("heap %%.1fK");', 1),
            ("src/plugProjectKandoU/gamePlayDataMemCard.cpp", r'output.textWriteText(textBuffer);', r'output.textWriteText("%s", textBuffer);', 3),
            ("src/plugProjectKandoU/pikiContainer.cpp", r'output.textWriteText(buffer);', r'output.textWriteText("%s", buffer);', 1),
            ("src/plugProjectOgawaU/ogScreen.cpp", r'JUT_PANICLINE(688, errCode);', r'JUT_PANICLINE(688, "%s", errCode);', 1),
            ("src/sysGCU/bootSection.cpp", r'JUT_ASSERTLINE(786, file, buf);', r'JUT_ASSERTLINE(786, file, "%s", buf);', 1),
            ("src/sysGCU/dvdStatus.cpp", r'print.print(40.0f, 200.0f, errorMsgSet[mErrorIndex]);', r'print.print(40.0f, 200.0f, "%s", errorMsgSet[mErrorIndex]);', 1),
            ("src/sysGCU/graphics.cpp", r'(f32)info.mPerspectiveOffsetY, buf);', r'(f32)info.mPerspectiveOffsetY, "%s", buf);', 3),
            ("src/sysGCU/graphics.cpp", r'printer.getWidth(buf);', r'printer.getWidth("%s", buf);', 2),
            ("src/sysGCU/screenObj.cpp", r'print.print(100.0f, 100.0f, mName);', r'print.print(100.0f, 100.0f, "%s", mName);', 1),
    ):
        replace(path, old, new, count=count)
    # String-pool stubs for stripped functions: NUL-filled formats print nothing.
    for path in ("src/plugProjectEbisawaU/particle2dMgr.cpp", "src/plugProjectKandoU/vsGS_VSGame.cpp",
                 "src/plugProjectKonoU/khFinalFloor.cpp", "src/sysGCU/sysShapeAnimation.cpp"):
        replace(path, '\tOSReport("\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0");\n', "")
    # The banner is truncated to its 32-byte field on purpose, as on the console.
    replace("src/sysGCU/memoryCard.cpp", 'snprintf((char*)header + 0x1c00, 0x20, "ピクミン２　セーブデータ ");',
            'strncpy((char*)header + 0x1c00, "ピクミン２　セーブデータ ", 0x1f);\n\t((char*)header)[0x1c00 + 0x1f] = 0;')
    # Atan/asin lookups reach index LENGTH (x == y), which on the console read the
    # next field, mDebugUnitCircle[0] (pi/4). Give the table that entry legally.
    replace("include/JSystem/JMath.h", "\tT mTable[LENGTH];\n\tT mDebugUnitCircle[8];", "\tT mTable[LENGTH + 1];\n\tT mDebugUnitCircle[8];", count=2)
    file = destination / "include/JSystem/JMath.h"
    text, count = re.subn(r"(mDebugUnitCircle\[0\]\s*= TAngleConstant_<T>::RADIAN_DEG180\(\) / 4;)",
                          r"\1\n\t\tmTable[LENGTH] = mDebugUnitCircle[0];", file.read_text())
    if count != 2: raise RuntimeError("Lookup table patch drift")
    file.write_text(text)
    # FX line configs point straight into the big-endian AAF scene data (found by
    # UBSan: send index 7 read as 1792; buffer count 48 read as 0x30000000).
    replace("include/JSystem/JAudio/JAS/JASDsp.h",
            "\tu8 mStatus;       // _00\n\tu16 _02;          // _02\n\ts16 _04;          // _04\n\tu16 _06;          // _06\n"
            "\ts16 _08;          // _08\n\ts16 _0A;          // _0A\n\tu32 mBufferCount; // _0C\n\ts16 _10[8];       // _10\n",
            "\tu8 mStatus;               // _00\n\tP2Big<u16> _02;           // _02\n\tP2Big<s16> _04;           // _04\n"
            "\tP2Big<u16> _06;           // _06\n\tP2Big<s16> _08;           // _08\n\tP2Big<s16> _0A;           // _0A\n"
            "\tP2Big<u32> mBufferCount;  // _0C\n\tP2Big<s16> _10[8];        // _10, disc data\n")
    replace("include/JSystem/JAudio/JAS/JASDsp.h", '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"')
    # A pointer stepped one before the array then indexed +1 (UBSan, index -1).
    replace("src/JSystem/JAudio/JAS/JASBNKParser.cpp",
            "\tTOffset<TInst>* instOffsets = header->mInstOffsets - 1;\n\tfor (int i = 0; i < TPerc_MAX_ENTRIES; i++) {\n\t\tTInst* instRaw = instOffsets[i + 1].ptr(header);",
            "\tTOffset<TInst>* instOffsets = header->mInstOffsets;\n\tfor (int i = 0; i < TPerc_MAX_ENTRIES; i++) {\n\t\tTInst* instRaw = instOffsets[i].ptr(header);")
    # MixConfig splits a u16 into {u: high byte, l0: high nibble of the low byte,
    # l1: low nibble}. MWCC laid the struct out big-endian; on a little-endian
    # host the declared order read the wrong byte (bus index 0x50 for 0x150).
    replace("include/JSystem/JAudio/JAS/JASChannel.h", "\t\tstruct {\n\t\t\tu8 u;\n\t\t\tu8 l0 : 4;\n\t\t\tu8 l1 : 4;\n\t\t} mParts;",
            "\t\tstruct { // little-endian order of the console's big-endian layout\n\t\t\tu8 l1 : 4;\n\t\t\tu8 l0 : 4;\n\t\t\tu8 u;\n\t\t} mParts;")
    # The DSP channel has six output mixers (0x10-0x40); the decomp sized four,
    # so mixers 4 and 5 were written past the array into the padding.
    replace("include/JSystem/JAudio/JAS/JASDsp.h", "\tTMixer mMixer[4];         // _10\n\tu8 _30[0x20];             // _30",
            "\tTMixer mMixer[6];         // _10\n\tu8 _40[0x10];             // _40")
    # Registers 0-13 are the consecutive u16 fields from _00 to _1A; indexing
    # _00[] past 5 reached them by layout. Registers 16-23 overlay the 32-bit
    # registers _20[0..3]; on the big-endian console the even one is the high
    # half, and sequences write 16/17 then read register 0x28 as one value.
    replace("include/JSystem/JAudio/JAS/JASRegisterParam.h", "\tu16 _00[6]; // _00 - unknown",
            "\tu16& reg(int index)\n\t{\n\t\tif (index >= 16 && index < 24) index ^= 1; // big-endian halves of _20[]\n"
            "\t\treturn reinterpret_cast<u16*>(this)[index];\n\t}\n\tu16 _00[6]; // _00 - unknown")
    replace("src/JSystem/JAudio/JAS/JASTrack.cpp", "\t\tresult = mRegisterParam._00[reg];", "\t\tresult = mRegisterParam.reg(reg);")
    replace("src/JSystem/JAudio/JAS/JASTrack.cpp", "\tmRegisterParam._00[reg] = value;", "\tmRegisterParam.reg(reg) = value;")
    replace("src/JSystem/JAudio/JAS/JASTrack.cpp", "\tmRegisterParam._00[nextByte] = val23;", "\tmRegisterParam.reg(nextByte) = val23;")
    # A null `next` means "append": pass null rather than a member of null.
    replace("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", "\tmTree.insertChild(&next->mTree, &heap->mTree);",
            "\tmTree.insertChild(next ? &next->mTree : nullptr, &heap->mTree);")
    # Texture-pattern binding read the texture slot as byte 4 of the disc entry.
    # src/animation.cpp converts entries to host-order u16s, so byte 4 became the
    # padding (0xFF) and mPatternIds[255] was written 510 bytes past the heap
    # object: the J2D setAnimation heap corruption (pod icons, end of day 1).
    replace("src/JSystem/J2D/J2DMaterial.cpp",
            "mAnmPtr->mPatternIds[((u8*)(table->mData[(u32)i * 2]))[4]] = i; // NB: need to fix up this struct eventually",
            "mAnmPtr->mPatternIds[table[i].mData[1][0] >> 8] = i; // texture slot: high byte of the third u16")
    # UBSan replay findings (October 2). Members the constructors never set,
    # read before the first write (values 98-255 seen): defined as false.
    replace("src/utilityU/PSMainSide_ObjSound.cpp", "    , _B0(0.0f)\n{", "    , _B0(0.0f)\n    , mActive(false)\n{")
    replace("src/plugProjectKandoU/updateMgr.cpp", "    , mDoForceActive(false)\n{", "    , mDoForceActive(false)\n    , mIsActive(false)\n{")
    replace("include/Game/pelletMgr.h", "\t    : PelletState(PELSTATE_Goal)\n\t{\n\t}", "\t    : PelletState(PELSTATE_Goal)\n\t    , mStartSuck(false)\n\t{\n\t}")
    # onInit colours a fresh Piki before setting its kind (original order); the
    # kind is still construction garbage there. changeShape recolours it later.
    replace("src/plugProjectKandoU/piki.cpp", "\tmDefaultColor = pikiColors[getKind()];",
            "\tmDefaultColor = pikiColors[(u32)getKind() <= PikiColorCount ? getKind() : 0];")
    # Mixer config 0xFFFF (no bus) indexes the 12-entry table with 255.
    replace("src/JSystem/JAudio/JAS/JASDSPInterface.cpp", "\tmixer->mBusConnect                 = connect_table[connectType];",
            "\tmixer->mBusConnect                 = connectType < 12 ? connect_table[connectType] : 0;")
    # Zero-length fades divided by zero; arm64 gives 0, so keep that result.
    replace("src/JSystem/JUtility/JUTFader.cpp", "((++mTicksRun * 0xFF) / mTicksTarget)", "(mTicksTarget ? (++mTicksRun * 0xFF) / mTicksTarget : 0)", count=2)
    # Indirect TEV stage info: one per TEV stage (16 x 12 bytes); the decomp
    # declared 4 and padded the rest, so stages 4-15 indexed past the array.
    replace("include/JSystem/J2D/J2DMaterialFactory.h",
            "\tJ2DIndTevStageInfo mIndTevStageInfo[4];           // _68\n\tu8 _AC[0x90];                                     // _AC",
            "\tJ2DIndTevStageInfo mIndTevStageInfo[16];          // _68, one per TEV stage")
    # update() moves the inactive (hidden) movie window with an angle the
    # constructor never set: garbage became NaN in atan2 (UBSan). 90 degrees is
    # the closed position open() and close() start from.
    replace("src/sysGCU/movieMessage.cpp", "\tmCurrPosition    = Vector3f::zero;\n}", "\tmCurrPosition    = Vector3f::zero;\n\tmCurrAngle       = 90.0f;\n}")
    # More UBSan findings: the (id, bounds) picture constructor ran initinfo's
    # blend setup over an unset texture count (reading past mBlendColorRatio;
    # recomputed right after), and ActGotoSlot read a flag only init() ever sets.
    replace("src/JSystem/J2D/J2DPicture.cpp", "    : J2DPane(id, bounds)\n    , mPalette(nullptr)\n{\n\tinitinfo();",
            "    : J2DPane(id, bounds)\n    , mPalette(nullptr)\n{\n\tmTextureCount = 0;\n\tinitinfo();")
    replace("src/plugProjectKandoU/aiPrimitives.cpp", "ActGotoSlot::ActGotoSlot(Game::Piki* p)\n    : Action(p)\n{",
            "ActGotoSlot::ActGotoSlot(Game::Piki* p)\n    : Action(p)\n    , mIsFirstPiki(false)\n{")
    # The day-end item screen computed its scissor from panes not yet drawn (NaN
    # on the first frame, UBSan). The PAL release fixed it with mHasDrawn; use it.
    pal = "#if defined(VERSION_PAL)"
    pal_or_pc = "#if defined(VERSION_PAL) || defined(TARGET_PC)"
    replace("include/kh/khDayEndResult.h", pal + "                                     //\n\tbool mHasDrawn;",
            pal_or_pc + "\n\tbool mHasDrawn;")
    for context in ("\n\tmHasDrawn = false;", "\n\tmHasDrawn = true;", "\n\tif (mHasDrawn) {", "\n\t} else {\n\t\tmGXScissorTopY = mGXScissorBottomY = 0;"):
        replace("src/plugProjectKonoU/khDayEndResult.cpp", pal + context, pal_or_pc + context)
    # Non-void functions the original binary does not contain: the decomp body
    # is only an "UNUSED" comment, so the function would fall off its end.
    # Reaching one now panics by name; nothing links them today.
    unused = re.compile(r"^(?P<head>[A-Za-z_][^\n;{}#(]*?[ \t*&])?(?P<name>[\w:~]+|[\w:<>, \*]+::operator\S+)"
                        r"\((?P<args>[^;{}]*?)\)[ \t]*(?:const)?[ \t]*\n(?:[ \t]*//[^\n]*\n)?\{\n(?P<body>(?:[ \t]*//[^\n]*\n)+)\}", re.M)
    unused_count = 0
    for file in sorted(destination.rglob("*")):
        if file.suffix not in (".cpp", ".h") or "src/Dolphin" in file.as_posix():
            continue
        text = file.read_text(errors="replace")
        edits = []
        for m in unused.finditer(text):
            if "UNUSED" not in m["body"] or text.rfind("/*", 0, m.start()) > text.rfind("*/", 0, m.start()):
                continue
            head = m["head"] or ""
            if not head.strip():  # return type on the previous line (template specialisations)
                previous = text[:m.start()].rstrip("\n").rsplit("\n", 1)[-1].strip()
                head = "" if not previous or previous.startswith(("template", "*", "/", "}")) else previous
            if re.sub(r"\b(inline|static|virtual|extern)\b", "", head).strip() in ("", "void"):
                continue
            edits.append(m.end() - 1)
        for at in reversed(edits):
            text = text[:at] + "\tP2_UNUSED_FUNCTION();\n" + text[at:]
        if edits:
            file.write_text(text)
            unused_count += len(edits)
    if unused_count != 94:
        raise RuntimeError(f"Unused-function patch drift: {unused_count} bodies, expected 94")
    output.mkdir(parents=True, exist_ok=True)
    stamp.unlink(missing_ok=True)
    changed = sync_tree(destination, output)
    shutil.rmtree(destination)
    stamp.write_text(signature)
    print(f"Prepared Pikmin 2 {revision[:12]} at {output}; {changed} files changed; audio disabled")


if __name__ == "__main__":
    main()
