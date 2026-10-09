#!/usr/bin/env python3
"""Prepare a pinned Aurora checkout without modifying the reference repository."""
import argparse
import hashlib
import io
from pathlib import Path
import shutil
import subprocess
import tarfile
from sync_tree import sync_tree

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
# Single source of truth, shared with setup.sh in the published tree.
revision = (Path(__file__).resolve().parents[1] / 'aurora-revision.txt').read_text().strip()
signature = revision + ':' + hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
output = args.output.resolve()
stamp = output / '.p2-renderer'
if stamp.exists() and stamp.read_text() == signature:
    raise SystemExit(0)
# Patch a scratch tree, then mirror only changed files into the stable output.
args.output = output.with_name(output.name + '.staging')
shutil.rmtree(args.output, ignore_errors=True)
data = subprocess.check_output(['git', '-C', str(args.source), 'archive', revision])
args.output.mkdir(parents=True)
with tarfile.open(fileobj=io.BytesIO(data)) as archive:
    archive.extractall(args.output, filter='data')
# Project integration patches applied to a fresh pinned snapshot.
p = args.output / 'CMakeLists.txt'
s = p.read_text().replace('if (NOT CMAKE_CROSSCOMPILING)\n  enable_testing()',
                         'if (AURORA_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)\n  enable_testing()')
p.write_text(s)
# Native card storage needs real mount/unmount lifecycle rather than the
# upstream placeholders. Work buffers are console-only; file ownership is real.
p = args.output / 'lib/dolphin/card.cpp'
s = p.read_text()
assert s.count('if (!loadedCard) {') == 1
s = s.replace('if (!loadedCard) {', 'if (!loadedCard && !std::filesystem::exists(cardPaths[0])) {')
begin = s.index('s32 CARDMount(const s32 chan,')
end = s.index('s32 CARDOpen(', begin)
s = s[:begin] + """s32 CARDMount(const s32 chan, void* workArea [[maybe_unused]], CARDCallback detachCallback [[maybe_unused]]) {
  if (chan < 0 || chan >= 2) return CARD_RESULT_FATAL_ERROR;
  if (!std::filesystem::exists(cardPaths[chan])) return CARD_RESULT_NOCARD;
  const auto& card = GET_CARD(chan);
  card->close();
  if (!card->open(cardPaths[chan])) return CARD_RESULT_IOERROR;
  return static_cast<s32>(card->getError());
}
s32 CARDMountAsync(const s32 chan, void* workArea, const CARDCallback detachCallback,
                   const CARDCallback attachCallback) {
  const s32 result = CARDMount(chan, workArea, detachCallback);
  if (attachCallback) attachCallback(chan, result);
  return result;
}

""" + s[end:]
begin = s.index('s32 CARDUnmount(const s32 chan)')
end = s.index('s32 CARDGetCurrentMode(', begin)
s = s[:begin] + """s32 CARDUnmount(const s32 chan) {
  if (chan < 0 || chan >= 2) return CARD_RESULT_FATAL_ERROR;
  GET_CARD(chan)->close();
  return CARD_RESULT_READY;
}

""" + s[end:]
p.write_text(s)
# Latch VI black state with each submitted frame. Clearing the swapchain while
# suppressing blits/overlays preserves EFB rendering without showing it early.
p = args.output / 'lib/aurora.cpp'
s = p.read_text()
s = '#include <atomic>\nstatic std::atomic<bool> p2PresentBlack{true};\nextern "C" void p2_aurora_set_present_black(bool black) { p2PresentBlack.store(black); }\n' + s
old = 'gfx::end_frame([rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,'
assert s.count(old) == 1
s = s.replace(old, 'gfx::end_frame([p2Black = p2PresentBlack.load(), rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,')
old = '        pass.Draw(3);\n        if (rmlBindGroup && rmlOverlay) {'
assert s.count(old) == 1
s = s.replace(old, '        if (!p2Black) pass.Draw(3);\n        if (!p2Black && rmlBindGroup && rmlOverlay) {')
old = '        imgui::render(pass, imguiDrawData);'
assert s.count(old) == 1
s = s.replace(old, '        if (!p2Black) imgui::render(pass, imguiDrawData);')
p.write_text(s)
# Preserve diagnostic draw-sync tokens in FIFO order. As with Aurora's
# draw-done callback, this boundary is command processing, not GPU completion.
def patch_renderer(path, old, new):
    file = args.output / path
    text = file.read_text()
    if text.count(old) != 1: raise RuntimeError(f"Renderer patch drift: {path}: {old!r}")
    file.write_text(text.replace(old, new))
patch_renderer('lib/gx/command_processor.hpp', '  bool drawDone;',
               '  bool drawDone;\n  bool drawSync = false;\n  uint16_t token = 0;')
patch_renderer('lib/gx/command_processor.cpp',
               '      const u32 value = reader.read<u32>();\n      handle_bp(value);',
               '''      const u32 value = reader.read<u32>();
      const u32 registerId = value >> 24;
      if (registerId == 0x48) {
        return {static_cast<u32>(reader.offset()), false, true, static_cast<u16>(value)};
      }
      if (registerId == 0x47) break; // Non-interrupt token register, paired with 0x48 by GXSetDrawSync.
      handle_bp(value);''')
patch_renderer('lib/gx/fifo.hpp', 'using DrawDoneCallback = void (*)();',
               'using DrawSyncCallback = void (*)(uint16_t);\nDrawSyncCallback set_draw_sync_callback(DrawSyncCallback callback) noexcept;\n\nusing DrawDoneCallback = void (*)();')
patch_renderer('lib/gx/fifo.cpp', 'std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};',
               'std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};\nstd::atomic<DrawSyncCallback> sDrawSyncCallback{nullptr};')
patch_renderer('lib/gx/fifo.cpp', '    if (result.drawDone) {\n      dispatch_draw_done();\n    }',
               '''    if (result.drawDone) {
      dispatch_draw_done();
    }
    if (result.drawSync) {
      if (const auto callback = sDrawSyncCallback.load(std::memory_order_acquire)) callback(result.token);
    }''')
patch_renderer('lib/gx/fifo.cpp', 'DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept {',
               '''DrawSyncCallback set_draw_sync_callback(DrawSyncCallback callback) noexcept {
  return sDrawSyncCallback.exchange(callback, std::memory_order_acq_rel);
}

DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept {''')
# The SDK decoder API has no input/output sizes. The native player uses only
# this bounded variant, including entropy reads and tile-storage capacities.
thp = 'lib/dolphin/thp/THPDec.cpp'
patch_renderer(thp, 's32 parse_headers(const void* file, DecodeContext& context) noexcept {\n  auto reader = aurora::ByteReader::unbounded(file);',
    's32 parse_headers(const void* file, size_t size, DecodeContext& context) noexcept {\n  aurora::ByteReader reader{static_cast<const u8*>(file), size};')
patch_renderer(thp, 'BitReader(const u8* data, size_t byteOffset) noexcept : mData{data}, mBitPosition{byteOffset * 8} {}',
    'BitReader(const u8* data, size_t size, size_t byteOffset) noexcept : mData{data}, mBitPosition{byteOffset * 8}, mSize{size} {}\n  bool valid() const noexcept { return !mFailed; }')
patch_renderer(thp, '      const size_t bytePosition = mBitPosition >> 3;',
    '      const size_t bytePosition = mBitPosition >> 3;\n      if (bytePosition >= mSize) { mFailed = true; return 0; }')
patch_renderer(thp, '  size_t mBitPosition;', '  size_t mBitPosition;\n  size_t mSize;\n  bool mFailed = false;')
patch_renderer(thp, '  if (!decode_block(reader, context, component, coefficients)) {',
    '  if (!decode_block(reader, context, component, coefficients) || !reader.valid()) {')
patch_renderer(thp, 's32 THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void*) {',
    '''s32 p2_thp_decode(const void* file, size_t size, void* tileY, size_t ySize,
                  void* tileU, size_t uSize, void* tileV, size_t vSize,
                  u32 expectedWidth, u32 expectedHeight) {''')
patch_renderer(thp, '  const s32 headerResult = parse_headers(file, context);',
    '  const s32 headerResult = parse_headers(file, size, context);')
patch_renderer(thp, '  const u16 chromaWidth = static_cast<u16>((context.width + 1) / 2);',
    '''  if (context.width != expectedWidth || context.height != expectedHeight ||
      context.width > 1024 || context.height > 1024) return kBadSyntax;
  const auto tiledSize = [](size_t w, size_t h) { return ((w + 7) / 8) * ((h + 3) / 4) * 32; };
  if (ySize < tiledSize(context.width, context.height) ||
      uSize < tiledSize((context.width + 1) / 2, (context.height + 1) / 2) ||
      vSize < tiledSize((context.width + 1) / 2, (context.height + 1) / 2)) return kNoOutput;
  const u16 chromaWidth = static_cast<u16>((context.width + 1) / 2);''')
patch_renderer(thp, '  BitReader bits{static_cast<const uint8_t*>(file), context.scanOffset};',
    '  BitReader bits{static_cast<const uint8_t*>(file), size, context.scanOffset};')
# Retail GXEnd is optional: fixed-count draw commands delimit themselves in
# the FIFO. Many original J2D/JPA/font draws omit it entirely. Preserve Aurora's
# mandatory end only for its native GX_AUTO extension, which patches a length.
patch_renderer('lib/dolphin/gx/GXVert.cpp',
    '  CHECK(!sInBegin, "GXBegin: called without matching GXEnd");',
    '''  if (sInBegin) {
    CHECK(!sBeginAuto, "GXBegin: GX_AUTO requires matching GXEnd");
    sInBegin = false;
    // Any intervening state commands are already after the self-delimited
    // vertex payload. Publish this complete command boundary without treating
    // those state bytes as vertices in GXEnd's optional diagnostic heuristic.
    aurora::gx::fifo::finish_draw();
  }''')
# J3D records hardware BP texture commands directly, without GXLoadTexObj's
# Aurora metadata command. Resolve MEM1 image addresses and invalidate native
# object identities so a preceding J2D texture cannot survive a J3D binding.
patch_renderer('lib/gx/regs.cpp', '#include "regs.hpp"', '#include "regs.hpp"\n#include <dolphin/os.h>')
patch_renderer('lib/gx/regs.cpp',
    '  auto& slot = g_gxState.loadedTextures[texMapId];\n  switch (kind) {',
    '  auto& slot = g_gxState.loadedTextures[texMapId];\n  slot.texObjId = 0;\n  slot.texDataVersion = 0;\n  switch (kind) {')
patch_renderer('lib/gx/regs.cpp',
    '    slot.mode0 = value;\n    break;',
    '    slot.mode0 = value;\n    slot.flags = (slot.flags & ~1u) | ((reg_get(value, 3, 5) & 3u) != 0 ? 1u : 0u);\n    break;')
patch_renderer('lib/gx/regs.cpp',
    '    slot.image3 = value;\n    break;',
    '    slot.image3 = value;\n    slot.data = reg_get(value, 24, 0) ? OSPhysicalToCached(reg_get(value, 24, 0) << 5) : nullptr;\n    break;')
# The maximum GX image dimension is 1024, encoded as 1023. Do not
# wrap that decoded size back to zero.
patch_renderer('lib/gfx/texture.hpp', 'get_bits(image0, 10, 0) + 1 & 0x3FF', 'get_bits(image0, 10, 0) + 1')
patch_renderer('lib/gfx/texture.hpp', 'get_bits(image0, 10, 10) + 1 & 0x3FF', 'get_bits(image0, 10, 10) + 1')
# SDK GXSetChanAmbColor/GXSetChanMatColor: GX_COLORn replaces only RGB and
# GX_ALPHAn only alpha. Aurora replaced the whole register, so J2DPictureEx's
# alpha-only GX_ALPHA0 update zeroed material RGB (black "Press Start").
for kind, reg in (('amb', 'A'), ('mat', 'C')):
    patch_renderer('lib/dolphin/gx/GXLighting.cpp',
        f"""  u32 packed = (static_cast<u32>(color.r) << 24) | (static_cast<u32>(color.g) << 16) |
               (static_cast<u32>(color.b) << 8) | static_cast<u32>(color.a);
  if (id == GX_COLOR0 || id == GX_ALPHA0) {{
    __gx->{kind}Color[0] = packed;""",
        f"""  const u32 chan = (id == GX_COLOR0 || id == GX_ALPHA0) ? 0 : 1;
  const u32 rgb = (static_cast<u32>(color.r) << 24) | (static_cast<u32>(color.g) << 16) |
                  (static_cast<u32>(color.b) << 8);
  const u32 packed = (id == GX_COLOR0 || id == GX_COLOR1) ? (rgb | (__gx->{kind}Color[chan] & 0xFFu))
                                                          : ((__gx->{kind}Color[chan] & ~0xFFu) | color.a);
  if (chan == 0) {{
    __gx->{kind}Color[0] = packed;""")
# The port owns ARAM (src/aram_memory.cpp maps console MEM1 addresses for DMA).
# Aurora's duplicate AR.cpp won link order for some symbols, so ARInit set up
# Aurora's store while reads/writes used the port's unallocated one.
patch_renderer('cmake/aurora_os.cmake', '        lib/dolphin/os/OSReport.cpp\n        lib/dolphin/AR.cpp)', '        lib/dolphin/os/OSReport.cpp)')
# Alpha-stage compares other than A8 test the colour stage's A and B inputs (Dolphin TevStageCombiner).
# Capture them before the colour stage writes its output register, as hardware evaluates both together.
patch_renderer('lib/gx/shader.cpp',
               '      std::string_view outReg = regName[stage.colorOp.outReg];\n      std::string op = tev_color_op(',
               '''      std::string_view outReg = regName[stage.colorOp.outReg];
      if (stage.alphaOp.op >= GX_TEV_COMP_R8_GT && stage.alphaOp.op <= GX_TEV_COMP_BGR24_EQ) {
        fragmentFn += fmt::format("\\n    let tev_alpha_cmp_a{0} = {1};\\n    let tev_alpha_cmp_b{0} = {2};", idx,
                                  color_arg(stage.colorPass.a), color_arg(stage.colorPass.b));
      }
      std::string op = tev_color_op(''')
patch_renderer('lib/gx/shader.cpp',
               'stage.alphaOp.clamp, alpha_arg(stage.alphaPass.a), alpha_arg(stage.alphaPass.b),',
               '''stage.alphaOp.clamp,
                                    stage.alphaOp.op >= GX_TEV_COMP_R8_GT && stage.alphaOp.op <= GX_TEV_COMP_BGR24_EQ
                                        ? fmt::format("tev_alpha_cmp_a{}", idx) : alpha_arg(stage.alphaPass.a),
                                    stage.alphaOp.op >= GX_TEV_COMP_R8_GT && stage.alphaOp.op <= GX_TEV_COMP_BGR24_EQ
                                        ? fmt::format("tev_alpha_cmp_b{}", idx) : alpha_arg(stage.alphaPass.b),''')
# Development frame dump (P2_DUMP_TICKS, src/input_record.cpp): the game asks for
# the frame it is building; end_frame copies the presented EFB source to a
# buffer, waits for it after submit and writes a binary PPM.
patch_renderer('lib/aurora.cpp', 'extern "C" void p2_aurora_set_present_black(bool black) { p2PresentBlack.store(black); }\n',
               """extern "C" void p2_aurora_set_present_black(bool black) { p2PresentBlack.store(black); }
#include <cstdio>
#include <mutex>
#include <string>
static std::mutex p2DumpMutex;
static std::string p2DumpPath;
extern "C" void p2_aurora_request_frame_dump(const char* path) { std::lock_guard lock{p2DumpMutex}; p2DumpPath = path; }
static std::string p2_take_frame_dump() { std::lock_guard lock{p2DumpMutex}; return std::exchange(p2DumpPath, {}); }
""")
patch_renderer('lib/aurora.cpp', 'gfx::end_frame([p2Black = p2PresentBlack.load(), ',
               'gfx::end_frame([p2Dump = p2_take_frame_dump(), p2Black = p2PresentBlack.load(), ')
patch_renderer('lib/aurora.cpp', '    webgpu::gpu_prof::frame_end(encoder);\n    const wgpu::CommandBufferDescriptor cmdBufDescriptor',
               """    wgpu::Buffer p2DumpBuffer;
    uint32_t p2DumpRow = 0, p2DumpWidth = 0, p2DumpHeight = 0;
    bool p2DumpBgra = false;
    if (!p2Dump.empty()) {
      const auto& source = webgpu::present_source();
      p2DumpWidth = source.size.width;
      p2DumpHeight = source.size.height;
      p2DumpRow = (p2DumpWidth * 4 + 255) & ~255u;
      p2DumpBgra = source.format == wgpu::TextureFormat::BGRA8Unorm || source.format == wgpu::TextureFormat::BGRA8UnormSrgb;
      const wgpu::BufferDescriptor descriptor{.label = "P2 frame dump",
                                              .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                              .size = uint64_t(p2DumpRow) * p2DumpHeight};
      p2DumpBuffer = webgpu::g_device.CreateBuffer(&descriptor);
      const wgpu::TexelCopyTextureInfo from{.texture = source.texture};
      const wgpu::TexelCopyBufferInfo to{.layout = {.bytesPerRow = p2DumpRow, .rowsPerImage = p2DumpHeight}, .buffer = p2DumpBuffer};
      const wgpu::Extent3D extent{p2DumpWidth, p2DumpHeight, 1};
      encoder.CopyTextureToBuffer(&from, &to, &extent);
    }
    webgpu::gpu_prof::frame_end(encoder);
    const wgpu::CommandBufferDescriptor cmdBufDescriptor""")
patch_renderer('lib/aurora.cpp', '    webgpu::gpu_prof::after_submit();\n',
               """    webgpu::gpu_prof::after_submit();
    if (p2DumpBuffer) {
      bool done = false, mapped = false;
      p2DumpBuffer.MapAsync(wgpu::MapMode::Read, 0, size_t(p2DumpRow) * p2DumpHeight, wgpu::CallbackMode::AllowProcessEvents,
                            [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                              done = true;
                              mapped = status == wgpu::MapAsyncStatus::Success;
                            });
      while (!done) webgpu::g_instance.ProcessEvents();
      FILE* file = mapped ? std::fopen(p2Dump.c_str(), "wb") : nullptr;
      if (file) {
        const auto* bytes = static_cast<const uint8_t*>(p2DumpBuffer.GetConstMappedRange(0, size_t(p2DumpRow) * p2DumpHeight));
        std::fprintf(file, "P6\\n%u %u\\n255\\n", p2DumpWidth, p2DumpHeight);
        std::vector<uint8_t> row(size_t(p2DumpWidth) * 3);
        for (uint32_t y = 0; y < p2DumpHeight; ++y) {
          const uint8_t* in = bytes + size_t(y) * p2DumpRow;
          for (uint32_t x = 0; x < p2DumpWidth; ++x) {
            row[x * 3 + 0] = in[x * 4 + (p2DumpBgra ? 2 : 0)];
            row[x * 3 + 1] = in[x * 4 + 1];
            row[x * 3 + 2] = in[x * 4 + (p2DumpBgra ? 0 : 2)];
          }
          std::fwrite(row.data(), 1, row.size(), file);
        }
        std::fclose(file);
        Log.info("Frame dump written: {}", p2Dump);
      } else {
        Log.warn("Frame dump failed: {}", p2Dump);
      }
      p2DumpBuffer.Unmap();
    }
""")
# Controller diagnostics: identity, raw capabilities and the SDL profile in use,
# so a misidentified pad (stick reported as D-pad) can be given a correct one.
patch_renderer('lib/input.cpp', '    g_GameControllers[instance] = controller;\n    apply_port_preferences();\n    return instance;',
               """    {
      SDL_Joystick* joystick = SDL_GetGamepadJoystick(ctrl);
      char guid[64] = {};
      SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, sizeof(guid));
      char* mapping = SDL_GetGamepadMapping(ctrl);
      Log.info("Controller {}: guid {}, {} axes, {} hats, {} buttons, path {}, mapping: {}", instance, guid,
               SDL_GetNumJoystickAxes(joystick), SDL_GetNumJoystickHats(joystick), SDL_GetNumJoystickButtons(joystick),
               SDL_GetGamepadPath(ctrl) ? SDL_GetGamepadPath(ctrl) : "none", mapping ? mapping : "none");
      SDL_free(mapping);
    }
    g_GameControllers[instance] = controller;
    apply_port_preferences();
    return instance;""")
# P2_NO_RUMBLE=1 suppresses controller rumble (diagnosing pads that disconnect on it).
patch_renderer('lib/input.cpp', '    SDL_RumbleGamepad(it->second.m_controller, low_freq_intensity, high_freq_intensity, duration_ms);',
               """    static const bool rumbleOff = std::getenv("P2_NO_RUMBLE") != nullptr;
    // The first rumble creates the system haptics engine (C++ inside
    // CoreHaptics). Host memory, or it lands in a game heap that is later reset.
    p2_host_scratch_push();
    if (!rumbleOff) SDL_RumbleGamepad(it->second.m_controller, low_freq_intensity, high_freq_intensity, duration_ms);
    p2_host_scratch_pop();""")
patch_renderer('lib/input.cpp', 'namespace aurora::input {',
               '// No-op unless the game links its host-scratch hooks (src/host_scratch.cpp).\nextern "C" __attribute__((weak)) void p2_host_scratch_push() {}\nextern "C" __attribute__((weak)) void p2_host_scratch_pop() {}\nnamespace aurora::input {')
# Offscreen benchmark (P2_BENCH): frames render and submit but never take a
# swapchain drawable, so the display's refresh no longer paces them.
patch_renderer('lib/aurora.cpp', 'static std::mutex p2DumpMutex;',
               'static std::atomic<bool> p2Offscreen{false};\nextern "C" void p2_aurora_set_offscreen(bool offscreen) { p2Offscreen.store(offscreen); }\nstatic std::mutex p2DumpMutex;')
patch_renderer('lib/aurora.cpp', '      if (window::is_presentable() && g_surface) {\n        ZoneScopedN("Acquire texture");',
               '      if (!p2Offscreen.load() && window::is_presentable() && g_surface) {\n        ZoneScopedN("Acquire texture");')
patch_renderer('lib/aurora.cpp', '    } else {\n      Log.info("Skipping present; window not presentable");\n    }',
               '    } else if (!p2Offscreen.load()) {\n      Log.info("Skipping present; window not presentable");\n    }')
# Touch overlay (src/touch_draw.cpp) draws last in the final pass, over the
# game and ImGui, including while the frame is black (loads). The hook takes the
# wgpu::RenderPassEncoder as void* because it is declared before Aurora's headers.
patch_renderer('lib/aurora.cpp', 'static std::mutex p2DumpMutex;',
               'extern "C" { void (*p2_overlay_draw)(void* pass) = nullptr; }\nstatic std::mutex p2DumpMutex;')
patch_renderer('lib/aurora.cpp', '        if (!p2Black) imgui::render(pass, imguiDrawData);\n        pass.End();',
               '        if (!p2Black) imgui::render(pass, imguiDrawData);\n'
               '        if (p2_overlay_draw) p2_overlay_draw(const_cast<void*>(static_cast<const void*>(&pass)));\n'
               '        pass.End();')
# iOS: cap replacement textures at 1 GB, well under the app's memory limit.
# Eviction is least-recently-used; an evicted texture reloads on its next use.
patch_renderer('lib/gfx/texture_replacement.cpp',
               'constexpr uint64_t kReplacementCacheBudgetBytes = 4294967296; // 4GB',
               '#include <TargetConditionals.h>\n#if TARGET_OS_IPHONE\n'
               'constexpr uint64_t kReplacementCacheBudgetBytes = 1073741824; // 1GB\n#else\n'
               'constexpr uint64_t kReplacementCacheBudgetBytes = 4294967296; // 4GB\n#endif')
output.mkdir(parents=True, exist_ok=True)
stamp.unlink(missing_ok=True)
sync_tree(args.output, output)
shutil.rmtree(args.output)
stamp.write_text(signature)
