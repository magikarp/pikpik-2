// Portable SDK display calculations from the pinned decomp.
#include "Dolphin/gx.h"
#include <cmath>

static u32 __GXGetNumXfbLines(u32 height, u32 scale)
{
	u32 numLines;
	u32 actualHeight;
	u32 newScale;

	numLines     = (height - 1) * 0x100;
	actualHeight = (numLines / scale) + 1;

	newScale = scale;

	if (newScale > 0x80 && newScale < 0x100) {
		while (newScale % 2 == 0) {
			newScale /= 2;
		}

		if (height % newScale == 0) {
			actualHeight++;
		}
	}

	if (actualHeight > 0x400) {
		actualHeight = 0x400;
	}

	return actualHeight;
}

/**
 * @note Address: 0x800E5F1C
 * @note Size: 0x90
 */
u16 GXGetNumXfbLines(const u16 efbHeight, f32 yScale)
{
	u32 scale = (u32)(256.0f / yScale) & 0x1FF;

	return __GXGetNumXfbLines(efbHeight, scale);
}

/**
 * @note Address: 0x800E5FAC
 * @note Size: 0x238
 */
f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight)
{
	u32 scale;
	u32 height1;
	u32 height2;
	f32 scale2;
	f32 scale1;

	height1 = xfbHeight;
	scale1  = (f32)xfbHeight / (f32)efbHeight;
	scale   = (u32)(256.0f / scale1) & 0x1FF;
	height2 = __GXGetNumXfbLines(efbHeight, scale);

	while (height2 > xfbHeight) {
		height1--;
		scale1  = (f32)height1 / (f32)efbHeight;
		scale   = (u32)(256.0f / scale1) & 0x1FF;
		height2 = __GXGetNumXfbLines(efbHeight, scale);
	}

	scale2 = scale1;
	while (height2 < xfbHeight) {
		scale2 = scale1;
		height1++;
		scale1  = (f32)height1 / (f32)efbHeight;
		scale   = (u32)(256.0f / scale1) & 0x1FF;
		height2 = __GXGetNumXfbLines(efbHeight, scale);
	}

	return scale2;
}


void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, const Mtx44 proj)
{
	f32 x, nearZ, scale, sideX, dist;
	u32 i;

	if (proj[3][3] == 0.0) {
		nearZ = proj[2][3] / (proj[2][2] - 1.0f);
		sideX = nearZ / proj[0][0];
	} else {
		sideX = 1.0f / proj[0][0];
		nearZ = 1.7320508075688772935 * sideX;
	}

	scale = 2.0f / width;

	for (i = 0; i < ARRAY_SIZE(table->fogVals); i++) {
		x = (i + 1) * 32;
		x *= scale;
		x *= sideX;
		dist              = std::sqrt(1.0f + (x * x) / (nearZ * nearZ));
		table->fogVals[i] = (u32)(dist * 256.0f) & 0xFFF;
	}
}

