// touch_draw.cpp — rasteriser for the touch overlay (src/touch_pad.cpp).
//
// Pikmin 1's pc/src/pc_touch_draw.mm, moved from Metal to WebGPU so it can draw
// in Aurora's final pass (tools/prepare_renderer.py hooks p2_overlay_draw in
// after ImGui). The shader is the same SDF, line for line in WGSL:
//
//  - every control is one ROUNDED BOX evaluated as a signed distance field; a
//    circle or capsule is a rounded box whose radius saturates, so A, B, the
//    stick, the D-pad and the rotated X/Y kidneys are one shader;
//  - the outline is a second threshold on the same distance;
//  - the distance is in the layout's own unit space, so fwidth gives exact
//    antialiasing at any surface size;
//  - labels are a stroke font: each letter is a few segments, coverage is the
//    distance to the nearest one. Z comes from ac-metal's copy of this font.
//
// One instanced 4-vertex triangle strip per frame; the shapes travel in a
// uniform buffer (storage buffers are not available in fragment shaders on
// every WebGPU compatibility-mode device).
#include "p2_touch_pad.h"
#include "webgpu/gpu.hpp"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr int kMaxShapes = 32;
#define TD_STR2(x) #x
#define TD_STR(x)  TD_STR2(x)

// Mirrors TDInst / TDData in the shader: vec4-sized members, 16-byte aligned.
struct TDInst {
    float geo[4];  // cx, cy, hw, hh
    float sty[4];  // corner radius, outline width, rotation, label cap height
    float fill[4];
    float edge[4];
    float lcol[4];
    float g0[4];   // glyph ids 0..3, -1 = none
    float g1[4];   // glyph ids 4..7
};
struct TDData {
    float aspect[4];
    TDInst inst[kMaxShapes];
};

// Font metrics come from p2_touch_pad.h so the layout and the pen agree.
const std::string kShader = std::string(
    "const ADV: f32 = ") + TD_STR(P2_TOUCH_GLYPH_ADV) + ";\n"
    "const INK: f32 = " + TD_STR(P2_TOUCH_GLYPH_INK) + ";\n" + R"(
const STROKE: f32 = 0.070;
const BEAR: f32 = 0.070;
struct TDInst { geo: vec4f, sty: vec4f, fill: vec4f, edge: vec4f, lcol: vec4f, g0: vec4f, g1: vec4f };
struct TDData { aspect: vec4f, inst: array<TDInst, 32> };
@group(0) @binding(0) var<uniform> data: TDData;

// Stroke font: x 0..0.60, y 0 (cap top) to 1 (baseline), in P2_TOUCH_GLYPH_SET
// order A B L R S T X Y Z.
var<private> kSeg: array<vec4f, 43> = array<vec4f, 43>(
  vec4f(0.00,1.00,0.30,0.00), vec4f(0.30,0.00,0.60,1.00), vec4f(0.10,0.68,0.50,0.68),
  vec4f(0.06,0.00,0.06,1.00), vec4f(0.06,0.00,0.36,0.00), vec4f(0.36,0.00,0.52,0.12),
  vec4f(0.52,0.12,0.52,0.34), vec4f(0.52,0.34,0.36,0.47), vec4f(0.36,0.47,0.06,0.47),
  vec4f(0.36,0.47,0.55,0.61), vec4f(0.55,0.61,0.55,0.86), vec4f(0.55,0.86,0.36,1.00),
  vec4f(0.36,1.00,0.06,1.00),
  vec4f(0.08,0.00,0.08,1.00), vec4f(0.08,1.00,0.55,1.00),
  vec4f(0.06,0.00,0.06,1.00), vec4f(0.06,0.00,0.36,0.00), vec4f(0.36,0.00,0.52,0.13),
  vec4f(0.52,0.13,0.52,0.35), vec4f(0.52,0.35,0.36,0.48), vec4f(0.36,0.48,0.06,0.48),
  vec4f(0.30,0.48,0.58,1.00),
  vec4f(0.56,0.14,0.44,0.02), vec4f(0.44,0.02,0.19,0.02), vec4f(0.19,0.02,0.06,0.14),
  vec4f(0.06,0.14,0.06,0.34), vec4f(0.06,0.34,0.19,0.45), vec4f(0.19,0.45,0.42,0.53),
  vec4f(0.42,0.53,0.55,0.65), vec4f(0.55,0.65,0.55,0.86), vec4f(0.55,0.86,0.42,0.98),
  vec4f(0.42,0.98,0.15,0.98), vec4f(0.15,0.98,0.04,0.86),
  vec4f(0.00,0.02,0.60,0.02), vec4f(0.30,0.02,0.30,1.00),
  vec4f(0.03,0.00,0.57,1.00), vec4f(0.57,0.00,0.03,1.00),
  vec4f(0.03,0.00,0.30,0.52), vec4f(0.57,0.00,0.30,0.52), vec4f(0.30,0.52,0.30,1.00),
  vec4f(0.02,0.02,0.58,0.02), vec4f(0.58,0.02,0.02,0.98), vec4f(0.02,0.98,0.58,0.98));
var<private> kFirst: array<i32, 9> = array<i32, 9>(0, 3, 13, 15, 22, 33, 35, 37, 40);
var<private> kCount: array<i32, 9> = array<i32, 9>(3, 10, 2, 7, 11, 2, 2, 3, 3);

fn sd_box(p: vec2f, b: vec2f, r: f32) -> f32 {
  let q = abs(p) - b + r;
  return length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - r;
}
fn sd_seg(p: vec2f, a: vec2f, b: vec2f) -> f32 {
  let pa = p - a;
  let ba = b - a;
  let h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
  return length(pa - ba * h);
}
struct VOut { @builtin(position) pos: vec4f, @location(0) local: vec2f, @location(1) @interpolate(flat) iid: u32 };

@vertex
fn td_v(@builtin(vertex_index) vid: u32, @builtin(instance_index) iid: u32) -> VOut {
  let s = data.inst[iid];
  // Grow past the outline by enough for the antialias feather.
  let m = s.sty.y + 0.005;
  let h = s.geo.zw + vec2f(m);
  let c = vec2f(select(-h.x, h.x, (vid & 1u) != 0u), select(-h.y, h.y, (vid & 2u) != 0u));
  // rot turns local +x counter-clockwise ON SCREEN, and unit y points down.
  let ca = cos(s.sty.z);
  let sa = sin(s.sty.z);
  let u = vec2f(c.x * ca + c.y * sa, -c.x * sa + c.y * ca) + s.geo.xy;
  var o: VOut;
  o.pos = vec4f((u.x / data.aspect.x) * 2.0 - 1.0, 1.0 - u.y * 2.0, 0.0, 1.0);
  o.local = c;
  o.iid = iid;
  return o;
}

@fragment
fn td_f(in: VOut) -> @location(0) vec4f {
  let s = data.inst[in.iid];
  let b = s.geo.zw;
  let d = sd_box(in.local, b, clamp(s.sty.x, 0.0, min(b.x, b.y)));
  let aa = max(fwidth(d), 1e-5);
  var col = s.fill;
  if (s.sty.y > 0.0) {
    col = mix(s.fill, s.edge, smoothstep(-aa, aa, d + s.sty.y));
  }
  col.a = col.a * (1.0 - smoothstep(-aa, aa, d));
  let lh = s.sty.w;
  if (lh > 0.0) {
    let ids = array<f32, 8>(s.g0.x, s.g0.y, s.g0.z, s.g0.w, s.g1.x, s.g1.y, s.g1.z, s.g1.w);
    var n = 0;
    while (n < 8 && ids[n] >= 0.0) { n++; }
    if (n > 0) {
      let e = in.local / lh; // em units, centred on the label box
      let x0 = -0.5 * (f32(n - 1) * ADV + INK) + BEAR;
      var dmin = 1e9;
      for (var i = 0; i < n; i++) {
        let g = e - vec2f(x0 + f32(i) * ADV, -0.5);
        let gi = i32(ids[i]);
        let first = kFirst[gi];
        let cnt = kCount[gi];
        for (var k = 0; k < cnt; k++) {
          let sg = kSeg[first + k];
          dmin = min(dmin, sd_seg(g, sg.xy, sg.zw));
        }
      }
      let ga = (1.0 - smoothstep(-aa, aa, (dmin - STROKE) * lh)) * s.lcol.a;
      // Straight-alpha "over": the letter on the button face, then the pair
      // blended onto the scene as one colour.
      let outA = ga + col.a * (1.0 - ga);
      var rgb = col.rgb;
      if (outA > 1e-5) {
        rgb = (s.lcol.rgb * ga + col.rgb * col.a * (1.0 - ga)) / outA;
      }
      col = vec4f(rgb, outA);
    }
  }
  return col;
}
)";

wgpu::RenderPipeline pipeline;
wgpu::TextureFormat pipelineFormat = wgpu::TextureFormat::Undefined;
wgpu::Buffer uniforms;
wgpu::BindGroup bindGroup;
bool failed;

bool ensurePipeline(wgpu::TextureFormat format) {
    if (pipeline && pipelineFormat == format) return true;
    if (failed) return false;
    const auto& device = aurora::webgpu::g_device;
    wgpu::ShaderSourceWGSL source{};
    source.code = kShader.c_str();
    wgpu::ShaderModuleDescriptor moduleDescriptor{};
    moduleDescriptor.nextInChain = &source;
    moduleDescriptor.label = "Touch overlay";
    const auto module = device.CreateShaderModule(&moduleDescriptor);

    wgpu::BindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    entry.buffer.type = wgpu::BufferBindingType::Uniform;
    entry.buffer.minBindingSize = sizeof(TDData);
    wgpu::BindGroupLayoutDescriptor layoutDescriptor{};
    layoutDescriptor.entryCount = 1;
    layoutDescriptor.entries = &entry;
    const auto bindLayout = device.CreateBindGroupLayout(&layoutDescriptor);
    wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{};
    pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
    pipelineLayoutDescriptor.bindGroupLayouts = &bindLayout;
    const auto pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);

    wgpu::BlendState blend{};
    blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha, wgpu::BlendFactor::OneMinusSrcAlpha};
    blend.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha, wgpu::BlendFactor::OneMinusSrcAlpha};
    wgpu::ColorTargetState target{};
    target.format = format;
    target.blend = &blend;
    wgpu::FragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = "td_f";
    fragment.targetCount = 1;
    fragment.targets = &target;
    wgpu::RenderPipelineDescriptor descriptor{};
    descriptor.label = "Touch overlay";
    descriptor.layout = pipelineLayout;
    descriptor.vertex.module = module;
    descriptor.vertex.entryPoint = "td_v";
    descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleStrip;
    descriptor.fragment = &fragment;
    pipeline = device.CreateRenderPipeline(&descriptor);
    if (!pipeline) {
        std::fputs("[TOUCH] *** overlay pipeline failed; touch controls will not be drawn\n", stderr);
        failed = true;
        return false;
    }
    pipelineFormat = format;
    if (!uniforms) {
        wgpu::BufferDescriptor bufferDescriptor{};
        bufferDescriptor.label = "Touch overlay shapes";
        bufferDescriptor.size = sizeof(TDData);
        bufferDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        uniforms = device.CreateBuffer(&bufferDescriptor);
    }
    wgpu::BindGroupEntry bind{};
    bind.binding = 0;
    bind.buffer = uniforms;
    bind.size = sizeof(TDData);
    wgpu::BindGroupDescriptor bindDescriptor{};
    bindDescriptor.layout = bindLayout;
    bindDescriptor.entryCount = 1;
    bindDescriptor.entries = &bind;
    bindGroup = device.CreateBindGroup(&bindDescriptor);
    return true;
}

void pack(TDInst& d, const P2TouchShape& s) {
    d = {{s.cx, s.cy, s.hw, s.hh}, {s.radius, s.edgew, s.rot, s.lh}, {}, {}, {}, {}, {}};
    std::memcpy(d.fill, s.fill, sizeof(d.fill));
    std::memcpy(d.edge, s.edge, sizeof(d.edge));
    std::memcpy(d.lcol, s.lcol, sizeof(d.lcol));
    for (int i = 0; i < 4; ++i) { d.g0[i] = s.glyph[i]; d.g1[i] = s.glyph[i + 4]; }
}

void draw(void* passPointer) {
    P2TouchShape shapes[kMaxShapes];
    const int count = p2_touch_pad_get_shapes(shapes, kMaxShapes);
    if (count <= 0) return;
    const auto& surface = aurora::webgpu::g_graphicsConfig.surfaceConfiguration;
    if (surface.width == 0 || surface.height == 0 || !ensurePipeline(surface.format)) return;
    static TDData data;
    data.aspect[0] = float(surface.width) / float(surface.height);
    for (int i = 0; i < count; ++i) pack(data.inst[i], shapes[i]);
    aurora::webgpu::g_queue.WriteBuffer(uniforms, 0, &data, sizeof(data));
    const auto& pass = *static_cast<const wgpu::RenderPassEncoder*>(passPointer);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bindGroup, 0, nullptr);
    pass.Draw(4, uint32_t(count), 0, 0);
}
} // namespace

extern "C" void (*p2_overlay_draw)(void* pass); // Aurora's final pass (tools/prepare_renderer.py)
extern "C" void p2_touch_overlay_install() { p2_overlay_draw = draw; }
