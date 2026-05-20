// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gpu/PublicGpu.h
//
// Lane 07 — GPU abstraction public-header CONTRACT (Wave 0).
//
// One narrow API surface for Vulkan (Win/Linux) and native Metal (macOS
// Apple Silicon). Other lanes write API-NEUTRAL code that calls into
// psy::gpu::* and never touches vk* / MTL* directly. If a lane needs a
// feature missing here, file an Issue against lane 07.
//
// See DESIGN-PSYNDER-GX.md §4 (memory), §7 (renderer), §11 (platform).
//
// Rule (DESIGN §4.4 + §14): NO mid-frame GPU allocations. All Heap/Buffer
// /Texture creation happens at level load or startup. Render targets are
// aliased via the transient allocator (Heap::Transient).
//
// Rule (DESIGN §4.3): GPU resources are owned by psy::gpu::Handle<T>, an
// intrusive refcount with frames-in-flight tracking. NO std::shared_ptr
// outside this lane.

#pragma once

#include <cstddef>
#include <cstdint>

// PipelineHandle (lane 08) — opaque, but we name it in our signatures so
// render lanes can bind a compiled pipeline through the gpu encoder API.
// Pulled in directly here (rather than forward-declared) because the
// public surface takes it by value — definition must be visible.
#include "shader/PublicShader.h"

namespace psynder::gpu {

// ─── Forward decls / opaque handles ─────────────────────────────────────
struct Device;
struct Heap;
struct Buffer;
struct Texture;
struct Sampler;
struct Pipeline;
struct DescriptorSet;
struct CmdBuffer;
struct AccelerationStructure; // only valid when device_supports_rt()

// ─── Intrusive refcount handle with frames-in-flight tracking ───────────
template <typename T>
class Handle {
public:
    Handle() noexcept = default;
    explicit Handle(T* p) noexcept; // increment refcount
    Handle(const Handle&) noexcept;
    Handle(Handle&&) noexcept;
    Handle& operator=(const Handle&) noexcept;
    Handle& operator=(Handle&&) noexcept;
    ~Handle();
    T* operator->() const noexcept { return ptr_; }
    T& operator*()  const noexcept { return *ptr_; }
    T* get()        const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
private:
    T* ptr_ = nullptr;
};

// ─── Heap types (DESIGN §4.1) ───────────────────────────────────────────
enum class HeapKind : std::uint8_t {
    DeviceLocal,   // VRAM (or unified on Apple Silicon)
    HostVisible,   // CPU-visible mapped device memory
    Transient,     // memoryless / aliased per-frame attachments
    Descriptor,    // descriptor sets
    Command,       // command-buffer storage
};

// ─── Buffer / Texture descriptors ───────────────────────────────────────
enum class BufferUsage : std::uint32_t {
    Vertex      = 1u << 0,
    Index       = 1u << 1,
    Uniform     = 1u << 2,
    Storage     = 1u << 3,
    Indirect    = 1u << 4,
    Staging     = 1u << 5,
    RtScratch   = 1u << 6,
    RtAccel     = 1u << 7,
};

enum class Format : std::uint16_t {
    Undefined,
    Rgba8Unorm,
    Rgba8Srgb,
    Bgra8Unorm,       // linear swapchain-compatible format
    Bgra8Srgb,        // common swapchain format on macOS/Vulkan
    R8Unorm,          // single-channel (font glyph atlases, SDF masks)
    Rgba16Float,
    Rgba32Float,
    Depth32Float,
    Depth24UnormStencil8,
    Bc1Unorm, Bc3Unorm, Bc7Unorm,
    Astc4x4Unorm, Astc8x8Unorm,
    R32Uint,
};

enum class TextureUsage : std::uint32_t {
    Sampled        = 1u << 0,
    Storage        = 1u << 1,
    RenderTarget   = 1u << 2,
    DepthStencil   = 1u << 3,
    TransferSrc    = 1u << 4,
    TransferDst    = 1u << 5,
};

struct BufferDesc {
    std::size_t  size_bytes = 0;
    std::uint32_t usage     = 0; // bitmask of BufferUsage
    HeapKind     heap       = HeapKind::DeviceLocal;
    const char*  debug_name = nullptr;
};

struct TextureDesc {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::uint32_t depth  = 1;
    std::uint32_t mips   = 1;
    std::uint32_t array_layers = 1;
    Format        format = Format::Rgba8Srgb;
    std::uint32_t usage  = 0; // bitmask of TextureUsage
    HeapKind      heap   = HeapKind::DeviceLocal;
    const char*   debug_name = nullptr;

    // ─── Optional: synchronous mip-0 upload at create time ─────────────────
    //
    // M1 contract (DESIGN §4.4, no mid-frame GPU allocations applies to the
    // texture creation only — uploads bundled into create_texture run during
    // level load / startup, never inside the frame loop):
    //
    //   * initial_data: tightly-packed mip-0 pixel buffer (row pitch =
    //     width * bytes_per_pixel(format)). nullptr leaves the texture
    //     uninitialised (prior behaviour preserved).
    //   * initial_data_size: byte count of the buffer above. MUST equal
    //     width * height * bytes_per_pixel(format) — backends reject
    //     mismatches by returning an invalid Handle (no crash, log once).
    //
    // Supported M1 formats: Rgba8Unorm, Bgra8Unorm, R8Unorm. Anything else
    // (including SRGB variants and compressed BC*/ASTC*) is rejected at the
    // upload path — M3 will broaden this when the offline cooker (lane 09)
    // grows BC7 / ASTC pipelines. mip-1+ array slices and 3D textures are
    // also deferred to M3 (synchronous mip generation + cube map upload).
    //
    // Backend mapping:
    //   Metal (Apple Silicon, unified memory) — direct
    //     [id<MTLTexture> replaceRegion:mipmapLevel:withBytes:bytesPerRow:].
    //     No staging buffer needed; the texture's storage is host-visible.
    //   Vulkan — host-visible staging buffer + vkCmdCopyBufferToImage on a
    //     one-shot transfer command buffer, with UNDEFINED → TRANSFER_DST
    //     → SHADER_READ_ONLY layout transitions. Synchronous (waits before
    //     returning). M3 will batch uploads through the JobSystem.
    const void* initial_data      = nullptr;
    std::size_t initial_data_size = 0;
};

// ─── Device — top-level GPU handle ──────────────────────────────────────
struct DeviceDesc {
    bool enable_validation = false; // Vulkan validation layers in dev builds
    bool enable_rt         = true;  // gated by hardware capability detection
    bool enable_mesh_shaders = true;
    void* native_window_handle = nullptr; // HWND / NSWindow* / wl_surface*
};

Device* create_device(const DeviceDesc&);
void    destroy_device(Device*);

bool device_supports_rt(const Device*);
bool device_supports_mesh_shaders(const Device*);
bool device_is_unified_memory(const Device*); // true on Apple Silicon
const char* device_name(const Device*);

// ─── Resource creation (only outside the frame loop) ────────────────────
Handle<Buffer>  create_buffer(Device*, const BufferDesc&);
Handle<Texture> create_texture(Device*, const TextureDesc&);
Handle<Sampler> create_sampler(Device*); // default trilinear; details TBD

// Mid-frame helpers: map a HostVisible buffer, write, unmap. On Apple
// Silicon (unified memory), DeviceLocal buffers also support direct map.
void* buffer_map(Buffer*);
void  buffer_unmap(Buffer*);

// ─── Per-frame entry points ─────────────────────────────────────────────
// 1. Begin frame, acquire next swapchain image
bool   begin_frame(Device*);
// 2. Open a command buffer for the calling worker (one per worker)
CmdBuffer* cmd_open(Device*);
// 3. Submit + close
void       cmd_submit(Device*, CmdBuffer*);
// 4. End frame, present
void   end_frame(Device*);

// ─── Resize swapchain (when window/screen size changes) ─────────────────
void   resize_swapchain(Device*, std::uint32_t width, std::uint32_t height);

// ─── Pipeline / shader compile is owned by lane 08 (shader). See
//     PublicShader.h — pipelines are created from compiled shader blobs.

// ─── RT API surface (impl ships at M5; see PublicRenderRT.h for usage) ──
// Build / refit acceleration structures.
struct BlasDesc; struct TlasDesc;
Handle<AccelerationStructure> create_blas(Device*, const BlasDesc&);
Handle<AccelerationStructure> create_tlas(Device*, const TlasDesc&);
void refit_tlas(Device*, AccelerationStructure*);

// ─── Render-encoder API surface (lane09-001 / M1+ work) ─────────────────
//
// New surface added to unblock lane 09 / 11 / 21. The EXISTING per-frame
// entry points (begin_frame / cmd_open / cmd_submit / end_frame) are
// preserved — render lanes that don't use the encoder API still see the
// default backend behavior (animated clear color from cmd_submit).
//
// Usage shape inside a frame:
//
//   begin_frame(dev);
//   CmdBuffer* cb = cmd_open(dev);
//   begin_render(cb, pass);     // suppresses default auto-clear
//   set_viewport(cb, ...);
//   set_scissor(cb, ...);
//   bind_pipeline(cb, pipe);
//   bind_vertex_buffer(cb, 0, vb);
//   bind_index_buffer (cb, ib, IndexType::U16);
//   push_constants(cb, &uniforms, sizeof(uniforms), 1 | 2);
//   draw_indexed(cb, index_count);
//   end_render(cb);
//   cmd_submit(dev, cb);
//   end_frame(dev);
//
// If a frame opens a CmdBuffer and submits WITHOUT calling begin_render,
// the backend's auto-clear path runs (preserves M0 sample_00_clear).

// ─── Render pass attachment + viewport ─────────────────────────────────
enum class LoadOp  : std::uint8_t { Load, Clear, DontCare };
enum class StoreOp : std::uint8_t { Store, DontCare };

struct ColorAttachment {
    Texture*   target = nullptr;        // nullptr = use current swapchain image
    LoadOp     load   = LoadOp::Clear;
    StoreOp    store  = StoreOp::Store;
    float      clear_rgba[4] = {0, 0, 0, 1};
};
struct DepthAttachment {
    Texture* target = nullptr;
    LoadOp   load   = LoadOp::Clear;
    StoreOp  store  = StoreOp::DontCare;
    float    clear_depth = 1.0f;
    std::uint8_t clear_stencil = 0;
};
struct Viewport { float x, y, w, h, min_depth, max_depth; };
struct Scissor  { std::int32_t x, y; std::uint32_t w, h; };

struct RenderPassDesc {
    std::uint32_t  color_count = 1;
    ColorAttachment colors[8];
    DepthAttachment depth;       // target==nullptr disables depth attachment
    bool           swapchain    = true; // true = render to swapchain image
};

// ─── Index type + shader stage bits ────────────────────────────────────
enum class IndexType : std::uint8_t { U16, U32 };

// Shader stage bits used by push_constants. The values intentionally match
// the comment in the lane09-001 spec (1=vs 2=fs 4=cs 8=mesh) and a subset
// of VK_SHADER_STAGE_*_BIT values (so they pass straight through on the
// Vulkan side without a translation step).
namespace ShaderStage {
    constexpr std::uint32_t Vertex   = 1u; // VK_SHADER_STAGE_VERTEX_BIT
    constexpr std::uint32_t Fragment = 2u; // VK_SHADER_STAGE_FRAGMENT_BIT
    constexpr std::uint32_t Compute  = 4u; // a stand-in; VK_SHADER_STAGE_COMPUTE_BIT=0x20
    constexpr std::uint32_t Mesh     = 8u; // a stand-in for VK_SHADER_STAGE_MESH_BIT_EXT
    constexpr std::uint32_t AllGfx   = Vertex | Fragment;
} // namespace ShaderStage

// Max inline push-constant size. Matches Vulkan's guaranteed minimum and
// fits comfortably inside Metal's setVertexBytes / setFragmentBytes
// constant-data path (4 KiB ceiling).
constexpr std::uint32_t kMaxPushConstantBytes = 128;

// ─── Render-pass scope ─────────────────────────────────────────────────
void begin_render(CmdBuffer*, const RenderPassDesc&);
void end_render  (CmdBuffer*);

// ─── Inside a render pass ──────────────────────────────────────────────
void set_viewport(CmdBuffer*, const Viewport&);
void set_scissor (CmdBuffer*, const Scissor&);

void bind_pipeline(CmdBuffer*, ::psynder::shader::PipelineHandle);
void bind_vertex_buffer(CmdBuffer*, std::uint32_t binding, Buffer*, std::uint64_t offset = 0);
void bind_index_buffer (CmdBuffer*, Buffer*, IndexType, std::uint64_t offset = 0);

// Inline push constants (Vulkan vkCmdPushConstants /
// Metal setVertexBytes:atIndex: + setFragmentBytes:atIndex:).
// Max size: 128 bytes (matches Vulkan minimum guaranteed).
void push_constants(CmdBuffer*, const void* data, std::uint32_t size,
                    std::uint32_t stage_mask /*see ShaderStage::*/);

void draw         (CmdBuffer*, std::uint32_t vertex_count,
                              std::uint32_t instance_count = 1,
                              std::uint32_t first_vertex   = 0,
                              std::uint32_t first_instance = 0);
void draw_indexed (CmdBuffer*, std::uint32_t index_count,
                              std::uint32_t instance_count = 1,
                              std::uint32_t first_index    = 0,
                              std::int32_t  vertex_offset  = 0,
                              std::uint32_t first_instance = 0);

// ─── Compute (outside any render pass) ─────────────────────────────────
void dispatch(CmdBuffer*, std::uint32_t gx, std::uint32_t gy, std::uint32_t gz);

} // namespace psynder::gpu
