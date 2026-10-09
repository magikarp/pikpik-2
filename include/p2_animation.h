#pragma once
#include <cstddef>
struct J3DAnmTransform;
// ANK1/ANF1 block loader. Leaves the old animation intact on invalid input.
bool p2_load_transform(J3DAnmTransform* animation, const void* block, std::size_t size, bool key);
struct J3DAnmTevRegKey;
// TRK1 material color keys, including owned material names and writable IDs.
bool p2_load_tev_animation(J3DAnmTevRegKey* animation, const void* block, std::size_t size);
struct J3DAnmTextureSRTKey;
// TTK1 texture transforms and material bindings, including post-transform data.
bool p2_load_texture_animation(J3DAnmTextureSRTKey* animation, const void* block, std::size_t size);
struct J2DAnmTransform;
bool p2_load_2d_transform(J2DAnmTransform* animation, const void* block, std::size_t size, bool key);
struct J2DAnmColorKey;
bool p2_load_2d_color(J2DAnmColorKey* animation, const void* block, std::size_t size);
struct J2DAnmTextureSRTKey;
// TTK1 texture transforms bound to J2D materials (wraps the J3D loader).
bool p2_load_2d_texture_srt(J2DAnmTextureSRTKey* animation, const void* block, std::size_t size);
struct J2DAnmTevRegKey;
// TRK1 TEV colour/konst register animations bound to J2D materials.
bool p2_load_2d_tev_reg(J2DAnmTevRegKey* animation, const void* block, std::size_t size);
struct J3DAnmColorKey;
// PAK1 material colour keys for J3D models (same format as the 2D keys).
bool p2_load_color_key(J3DAnmColorKey* animation, const void* block, std::size_t size);
struct J2DAnmTexPattern;
struct J2DAnmVisibilityFull;
bool p2_load_2d_pattern(J2DAnmTexPattern* animation, const void* block, std::size_t size);
bool p2_load_2d_visibility(J2DAnmVisibilityFull* animation, const void* block, std::size_t size);
