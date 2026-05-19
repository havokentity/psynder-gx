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
    Bgra8Srgb,        // common swapchain format on macOS/Vulkan
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

} // namespace psynder::gpu
