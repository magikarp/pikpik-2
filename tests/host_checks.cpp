#include "types.h"
#include "JSystem/JMath.h"
#include "JSystem/JSupport/JSU.h"
#include "Dolphin/OS/OSFastCast.h"
#include "Dolphin/GX/GXTypes.h"

static int failures;
static void check(bool condition, const char* message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
int main() {
    check(sizeof(u32) == 4 && sizeof(s32) == 4, "GameCube words stay 32-bit");
    check(sizeof(void*) == 8, "host pointers are 64-bit");
    check(sizeof(GXTexObj) == 64 && sizeof(GXTlutObj) == 40,
          "runtime texture objects have room for Aurora's native pointers");
    const uintptr_t highAddress = UINT64_C(0x123456780123);
    check(ALIGN_PREV(highAddress, 32u) == UINT64_C(0x123456780120), "alignment keeps high address bits");
    check(ALIGN_NEXT(highAddress, 32u) == UINT64_C(0x123456780140), "round-up keeps high address bits");
    check(sizeof(Vec) == 12 && sizeof(Mtx) == 48, "vector and matrix layout");
    check(P2_AUDIO_ENABLED == 0, "audio is disabled in code");
    check(__OSf32tos16(-12.75f) == -12 && __OSf32tos16(40000.0f) == 32767,
          "native signed fast-cast truncates and saturates");
    check(__OSf32tou8(-1.0f) == 0 && __OSf32tou8(400.0f) == 255 && __OSf32tou8(17.9f) == 17,
          "native byte fast-cast saturates");
    check(__OSf32tos8(-200.0f) == -128 && __OSf32tos8(200.0f) == 127,
          "native signed byte fast-cast saturates");
    u8 buffer[32] = {};
    check(JSUConvertOffsetToPtr<u8>(buffer, (u32)7) == buffer + 7, "offset conversion preserves host address");
    check(JSUConvertOffsetToPtr<u8>(buffer, (u32)0) == 0, "zero resource offset means null");
    Vec a = {1,2,3}, b = {4,5,6};
    check(JMathInlineVEC::PSVECDotProduct(&a, &b) == 32, "paired-single dot replacement");
    JMathInlineVEC::PSVECAdd(&a, &b, &a);
    check(a.x == 5 && a.y == 7 && a.z == 9, "aliased vector addition");
    JMathInlineVEC::PSVECSubtract(&a, &b, &a);
    JMathInlineVEC::PSVECScale(&a, &a, 2);
    check(JMathInlineVEC::PSVECSquareMag(&a) == 56, "vector subtract, scale and magnitude");
    check(fabs(JMath::sincosTable_.sinShort(16384) - 1) < 0.00001, "native JSystem sine table");
    check(fabs(JMath::atanTable_.atan2_(1,1) - QUARTER_PI) < 0.00001, "native JSystem atan table");
    check(fabs(JMath::sincosTable_.sinRadian(HALF_PI) - 1) < 0.00001, "radian sine index mask");
    check(fabs(JMath::sincosTable_.sinRadian(-HALF_PI) + 1) < 0.00001, "negative radian sine");
    JMath::TRandom_fast_ random(0xffffffffu);
    check(random.currentFloat_0_1() >= 0 && random.currentFloat_0_1() < 1, "random bit pattern is data, not an address");
    check(random.next() == 0x3c558d52u, "32-bit random state wraps like GameCube");
    u8 texture[128] = {}, copy[32] = {}, secondCopy[32] = {};
    p2_texture_copy(copy, texture, 32, 64);
    check(p2_texture_image(copy, 32) == texture+32, "texture copy retains source host address");
    check(p2_texture_palette(copy, 64) == texture+64, "texture palette retains source host address");
    p2_texture_copy(secondCopy, copy, 32, 64);
    check(p2_texture_image(secondCopy, 32) == texture+32, "texture copy of a copy resolves original data");
    p2_texture_forget(copy);
    check(p2_texture_image(copy, 0) == copy, "texture address reuse clears old mapping");
    p2_texture_forget(secondCopy);
    printf("Pikmin 2 native layout/math checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
