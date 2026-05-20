// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/shader/PipelineCreateVulkan.cpp
//
// Lane 08 — Vulkan pipeline-state-object construction + lane-07 registration.
//
// Guarded by PSYNDER_GX_BACKEND_VULKAN. On macOS this TU compiles to
// effectively-empty (no Vulkan headers in the macOS toolchain).
//
// Flow:
//   1. Application / platform layer calls
//        psynder_gx_shader_push_vk_context(VkDevice, VkPhysicalDevice)
//      after gpu::create_device returns.
//   2. When create_graphics / create_compute is called and SPIR-V blobs
//      are ready, we construct a VkPipeline + VkPipelineLayout from the
//      blobs and hand them to lane 07 via the registration shim.
//   3. If the context hasn't been pushed yet (the orchestrator hasn't
//      yet wired the call from gpu::create_device), we queue the
//      compilation request and drain on push.
//
// Vulkan 1.3 dynamic rendering is used (lane 07's frame loop uses
// vkCmdBeginRendering). PSO creation supplies a VkPipelineRenderingCreateInfo
// with the swapchain format so the PSO is compatible with whatever
// dynamic-rendering pass lane 09 opens.
//
// Push-constant ABI: a single range, VK_SHADER_STAGE_ALL_GRAPHICS,
// offset 0, size kMaxPushConstantBytes (128). This matches lane 07's
// push_constants() encoder which uses VK_SHADER_STAGE_ALL_GRAPHICS too.

#include "shader/impl/PipelineCreateBackend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(PSYNDER_GX_BACKEND_VULKAN)

#include <volk.h>

// ─── Lane-07 registration shim (extern "C" symbol lives in
//     engine/gpu/vk/VulkanBackend.cpp) ──────────────────────────────────
extern "C" void psynder_gx_vk_register_pipeline(
    std::uint32_t id,
    void*         vk_pipeline,
    void*         vk_pipeline_layout,
    std::uint32_t bind_point /*0=gfx 1=compute*/);

namespace psynder::shader::impl {

namespace {

// Single-process context populated by psynder_gx_shader_push_vk_context.
// volk loads function pointers globally (volkInitialize + volkLoadDevice
// happen in lane 07), so any vk* call in this TU is resolvable once
// VkDevice is non-null.
struct VkContext {
    VkDevice         device      = VK_NULL_HANDLE;
    VkPhysicalDevice phys        = VK_NULL_HANDLE;
    // Swapchain color format. Lane 07 picks B8G8R8A8_UNORM by default;
    // we read it back here once the context is pushed so the PSO's
    // VkPipelineRenderingCreateInfo matches. Pushed alongside VkDevice
    // by the same setter — see the third parameter overload below.
    VkFormat         swapchain_color_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat         swapchain_depth_fmt = VK_FORMAT_D32_SFLOAT;
    bool             ready       = false;
};

VkContext         g_ctx;
std::mutex        g_ctx_mu;

// ─── Deferred-creation queue ──────────────────────────────────────────
// If lane 08 is asked to build a PSO before the VkDevice handshake has
// happened, we cache the SPIR-V + handle id and process on push.
struct DeferredGfx {
    std::uint32_t              id   = 0;
    std::vector<std::uint8_t>  vs;
    std::vector<std::uint8_t>  fs;
    std::string                vs_entry;
    std::string                fs_entry;
    // Vertex input layout (lane 09 / sample01-003).  attr_count == 0
    // signals "use DefaultVertexLayout".  Stored by value because
    // VertexInputDesc is POD with fixed-size arrays (no heap).
    VertexInputDesc            vi;
};
struct DeferredCs {
    std::uint32_t              id   = 0;
    std::vector<std::uint8_t>  cs;
    std::string                cs_entry;
};

std::vector<DeferredGfx> g_deferred_gfx;
std::vector<DeferredCs>  g_deferred_cs;

// Forward decls — used by the deferred-drain after push.
bool build_gfx_now(const DeferredGfx&);
bool build_cs_now (const DeferredCs&);

// ─── Helpers ───────────────────────────────────────────────────────────

// Map a public VertexAttrFormat to the Vulkan attribute format enum.
// Unknown values fall back to UNDEFINED so vkCreateGraphicsPipelines
// surfaces a clean validation error instead of an undefined-behavior
// cast.
VkFormat to_vk_format(VertexAttrFormat f) {
    switch (f) {
        case VertexAttrFormat::Float32:     return VK_FORMAT_R32_SFLOAT;
        case VertexAttrFormat::Float32x2:   return VK_FORMAT_R32G32_SFLOAT;
        case VertexAttrFormat::Float32x3:   return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexAttrFormat::Float32x4:   return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexAttrFormat::Uint8x4Norm: return VK_FORMAT_R8G8B8A8_UNORM;
        case VertexAttrFormat::Uint16x2:    return VK_FORMAT_R16G16_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkVertexInputRate to_vk_input_rate(VertexInputRate r) {
    return (r == VertexInputRate::Instance)
        ? VK_VERTEX_INPUT_RATE_INSTANCE
        : VK_VERTEX_INPUT_RATE_VERTEX;
}

VkShaderModule make_module(const std::vector<std::uint8_t>& spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    // SPIR-V is 32-bit-aligned. slangc always emits aligned blobs but
    // we still copy through a uint32_t-aligned vector to be defensive
    // about std::vector<uint8_t>::data() alignment guarantees.
    std::vector<std::uint32_t> words((spirv.size() + 3) / 4);
    std::memcpy(words.data(), spirv.data(), spirv.size());

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size();
    ci.pCode    = words.data();

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_ctx.device, &ci, nullptr, &mod) != VK_SUCCESS) {
        std::fputs("[psy::shader::vk] vkCreateShaderModule failed\n", stderr);
        return VK_NULL_HANDLE;
    }
    return mod;
}

VkPipelineLayout make_layout_with_push_constants() {
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = 128;

    VkPipelineLayoutCreateInfo lci{};
    lci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount         = 0;     // M1: no descriptor sets (M2 lane 09)
    lci.pSetLayouts            = nullptr;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcr;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(g_ctx.device, &lci, nullptr, &layout) != VK_SUCCESS) {
        std::fputs("[psy::shader::vk] vkCreatePipelineLayout failed\n", stderr);
        return VK_NULL_HANDLE;
    }
    return layout;
}

bool build_gfx_now(const DeferredGfx& d) {
    if (!g_ctx.ready || !g_ctx.device) return false;

    VkShaderModule vs_mod = make_module(d.vs);
    VkShaderModule fs_mod = make_module(d.fs);
    if (!vs_mod || !fs_mod) {
        if (vs_mod) vkDestroyShaderModule(g_ctx.device, vs_mod, nullptr);
        if (fs_mod) vkDestroyShaderModule(g_ctx.device, fs_mod, nullptr);
        return false;
    }

    VkPipelineLayout layout = make_layout_with_push_constants();
    if (!layout) {
        vkDestroyShaderModule(g_ctx.device, vs_mod, nullptr);
        vkDestroyShaderModule(g_ctx.device, fs_mod, nullptr);
        return false;
    }

    // Stage setup
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_mod;
    stages[0].pName  = d.vs_entry.c_str();
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_mod;
    stages[1].pName  = d.fs_entry.c_str();

    // ─── Vertex input ───────────────────────────────────────────────────
    // Two branches:
    //   (a) d.vi.attr_count == 0 — caller did not supply a layout, use
    //       DefaultVertexLayout (binding 0, stride 32, pos+normal+uv).
    //       This preserves every pre-lane-09 callsite that left
    //       GraphicsPipelineDesc::vertex_input default-constructed.
    //   (b) d.vi.attr_count  > 0 — translate VertexInputDesc into
    //       VkPipelineVertexInputStateCreateInfo. Attribute `location`
    //       is the attr's index in d.vi.attrs[]; this matches the
    //       `[[vk::location(N)]]` annotation lane 09's slang shaders use.
    VkVertexInputBindingDescription   vk_bindings[VertexInputDesc::kMaxBindings]{};
    VkVertexInputAttributeDescription vk_attrs   [VertexInputDesc::kMaxAttrs]{};
    std::uint32_t                     n_bindings = 0;
    std::uint32_t                     n_attrs    = 0;

    if (d.vi.attr_count == 0) {
        // Default layout: binding 0, stride 32, three interleaved attrs.
        vk_bindings[0].binding   = 0;
        vk_bindings[0].stride    = DefaultVertexLayout::kStrideBytes;
        vk_bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        n_bindings = 1;

        vk_attrs[0].location = 0;
        vk_attrs[0].binding  = 0;
        vk_attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        vk_attrs[0].offset   = DefaultVertexLayout::kAttribOffsets[0];
        vk_attrs[1].location = 1;
        vk_attrs[1].binding  = 0;
        vk_attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
        vk_attrs[1].offset   = DefaultVertexLayout::kAttribOffsets[1];
        vk_attrs[2].location = 2;
        vk_attrs[2].binding  = 0;
        vk_attrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
        vk_attrs[2].offset   = DefaultVertexLayout::kAttribOffsets[2];
        n_attrs = 3;
    } else {
        // Caller-supplied layout.  validate() rejects out-of-range
        // buffer_slot before we touch Vulkan; this guards the fixed-size
        // arrays + catches caller bugs early (assert in debug, log+leave
        // n_attrs=0 in release so vkCreateGraphicsPipelines fails cleanly).
        if (!validate(d.vi)) {
            std::fprintf(stderr,
                "[psy::shader::vk] VertexInputDesc failed validate() "
                "(id=%u, attr_count=%u, binding_count=%u)\n",
                d.id,
                (unsigned)d.vi.attr_count,
                (unsigned)d.vi.binding_count);
            vkDestroyShaderModule(g_ctx.device, vs_mod, nullptr);
            vkDestroyShaderModule(g_ctx.device, fs_mod, nullptr);
            vkDestroyPipelineLayout(g_ctx.device, layout, nullptr);
            return false;
        }
        for (std::uint8_t b = 0; b < d.vi.binding_count; ++b) {
            vk_bindings[b].binding   = b;
            vk_bindings[b].stride    = d.vi.bindings[b].stride;
            vk_bindings[b].inputRate = to_vk_input_rate(d.vi.bindings[b].input_rate);
        }
        n_bindings = d.vi.binding_count;

        for (std::uint8_t a = 0; a < d.vi.attr_count; ++a) {
            vk_attrs[a].location = a;
            vk_attrs[a].binding  = d.vi.attrs[a].buffer_slot;
            vk_attrs[a].format   = to_vk_format(d.vi.attrs[a].format);
            vk_attrs[a].offset   = d.vi.attrs[a].offset_in_buffer;
        }
        n_attrs = d.vi.attr_count;
    }

    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount   = n_bindings;
    vis.pVertexBindingDescriptions      = vk_bindings;
    vis.vertexAttributeDescriptionCount = n_attrs;
    vis.pVertexAttributeDescriptions    = vk_attrs;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport / scissor — dynamic. Lane 07 sets them via vkCmdSetViewport
    // before each draw.
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType        = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode  = VK_POLYGON_MODE_FILL;
    rs.cullMode     = VK_CULL_MODE_NONE;  // M1: no culling, deferred to M2
    rs.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth    = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dss{};
    dss.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable  = VK_FALSE;   // M1 forward pass to swapchain has no depth attachment
    dss.depthWriteEnable = VK_FALSE;
    dss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    att.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments    = &att;

    VkDynamicState dyn_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    // Vulkan 1.3 dynamic rendering — supply the color-attachment format
    // so the PSO is compatible with vkCmdBeginRendering(VkRenderingInfo)
    // that lane 07's begin_render opens.
    VkFormat color_fmts[1] = { g_ctx.swapchain_color_fmt };
    VkPipelineRenderingCreateInfo prc{};
    prc.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    prc.colorAttachmentCount    = 1;
    prc.pColorAttachmentFormats = color_fmts;
    prc.depthAttachmentFormat   = VK_FORMAT_UNDEFINED; // no depth for M1 swapchain

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.pNext               = &prc;
    gpi.stageCount          = 2;
    gpi.pStages             = stages;
    gpi.pVertexInputState   = &vis;
    gpi.pInputAssemblyState = &ias;
    gpi.pViewportState      = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &dss;
    gpi.pColorBlendState    = &cbs;
    gpi.pDynamicState       = &dyn;
    gpi.layout              = layout;
    gpi.renderPass          = VK_NULL_HANDLE;  // dynamic rendering, no VkRenderPass
    gpi.subpass             = 0;

    VkPipeline pso = VK_NULL_HANDLE;
    VkResult rc = vkCreateGraphicsPipelines(
        g_ctx.device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pso);

    // Shader modules can be destroyed immediately after pipeline creation
    // (PSO retains the IR internally per Vulkan spec).
    vkDestroyShaderModule(g_ctx.device, vs_mod, nullptr);
    vkDestroyShaderModule(g_ctx.device, fs_mod, nullptr);

    if (rc != VK_SUCCESS || pso == VK_NULL_HANDLE) {
        std::fprintf(stderr,
            "[psy::shader::vk] vkCreateGraphicsPipelines failed (rc=%d) id=%u\n",
            (int)rc, d.id);
        vkDestroyPipelineLayout(g_ctx.device, layout, nullptr);
        return false;
    }

    // Hand off to lane 07's bind-resolver registry.
    // Vulkan non-dispatchable handles are pointer-sized on 64-bit; the
    // shim's signature takes void* and reinterpret_casts on its side.
    psynder_gx_vk_register_pipeline(
        d.id,
        reinterpret_cast<void*>(pso),
        reinterpret_cast<void*>(layout),
        /*bind_point=*/0u /* graphics */);
    return true;
}

bool build_cs_now(const DeferredCs& d) {
    if (!g_ctx.ready || !g_ctx.device) return false;

    VkShaderModule cs_mod = make_module(d.cs);
    if (!cs_mod) return false;

    VkPipelineLayout layout = make_layout_with_push_constants();
    if (!layout) {
        vkDestroyShaderModule(g_ctx.device, cs_mod, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs_mod;
    stage.pName  = d.cs_entry.c_str();

    VkComputePipelineCreateInfo cpi{};
    cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage  = stage;
    cpi.layout = layout;

    VkPipeline pso = VK_NULL_HANDLE;
    VkResult rc = vkCreateComputePipelines(
        g_ctx.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pso);

    vkDestroyShaderModule(g_ctx.device, cs_mod, nullptr);

    if (rc != VK_SUCCESS || pso == VK_NULL_HANDLE) {
        std::fprintf(stderr,
            "[psy::shader::vk] vkCreateComputePipelines failed (rc=%d) id=%u\n",
            (int)rc, d.id);
        vkDestroyPipelineLayout(g_ctx.device, layout, nullptr);
        return false;
    }

    psynder_gx_vk_register_pipeline(
        d.id,
        reinterpret_cast<void*>(pso),
        reinterpret_cast<void*>(layout),
        /*bind_point=*/1u /* compute */);
    return true;
}

void drain_deferred_locked() {
    for (const auto& d : g_deferred_gfx) build_gfx_now(d);
    for (const auto& d : g_deferred_cs)  build_cs_now(d);
    g_deferred_gfx.clear();
    g_deferred_cs.clear();
}

} // anonymous namespace

bool create_and_register_graphics_pso(
    std::uint32_t                     handle_id,
    const std::vector<std::uint8_t>&  vs_blob,
    const std::vector<std::uint8_t>&  fs_blob,
    const char*                       vs_entry,
    const char*                       fs_entry,
    const VertexInputDesc&            vertex_input)
{
    if (vs_blob.empty() || fs_blob.empty()) return false;
    std::lock_guard<std::mutex> lock(g_ctx_mu);

    if (!g_ctx.ready) {
        // Queue; the push-setter will drain.
        DeferredGfx d;
        d.id       = handle_id;
        d.vs       = vs_blob;
        d.fs       = fs_blob;
        d.vs_entry = vs_entry ? vs_entry : "vs_main";
        d.fs_entry = fs_entry ? fs_entry : "fs_main";
        d.vi       = vertex_input;
        g_deferred_gfx.push_back(std::move(d));
        return true; // not failed — just pending
    }

    DeferredGfx d;
    d.id       = handle_id;
    d.vs       = vs_blob;
    d.fs       = fs_blob;
    d.vs_entry = vs_entry ? vs_entry : "vs_main";
    d.fs_entry = fs_entry ? fs_entry : "fs_main";
    d.vi       = vertex_input;
    return build_gfx_now(d);
}

bool create_and_register_compute_pso(
    std::uint32_t                     handle_id,
    const std::vector<std::uint8_t>&  cs_blob,
    const char*                       cs_entry)
{
    if (cs_blob.empty()) return false;
    std::lock_guard<std::mutex> lock(g_ctx_mu);

    if (!g_ctx.ready) {
        DeferredCs d;
        d.id       = handle_id;
        d.cs       = cs_blob;
        d.cs_entry = cs_entry ? cs_entry : "cs_main";
        g_deferred_cs.push_back(std::move(d));
        return true;
    }

    DeferredCs d;
    d.id       = handle_id;
    d.cs       = cs_blob;
    d.cs_entry = cs_entry ? cs_entry : "cs_main";
    return build_cs_now(d);
}

void release_registered_pso(std::uint32_t /*handle_id*/) {
    // M1 leak-by-design — see header. Real retire integrates with lane 07's
    // frames-in-flight reaper in M2.
}

} // namespace psynder::shader::impl

// ─── Public extern "C" setter the platform / app layer calls once
//     gpu::create_device returns. Lane 07 will gain a one-line call into
//     this from the end of its init() in a follow-up; for now the host
//     (sample / editor / platform) calls it directly. ────────────────────
extern "C" void psynder_gx_shader_push_vk_context(
    void*         vk_device,
    void*         vk_physical_device,
    std::uint32_t swapchain_color_format /* VkFormat enum value */)
{
    namespace impl = psynder::shader::impl;
    std::lock_guard<std::mutex> lock(impl::g_ctx_mu);
    impl::g_ctx.device              = reinterpret_cast<VkDevice>(vk_device);
    impl::g_ctx.phys                = reinterpret_cast<VkPhysicalDevice>(vk_physical_device);
    impl::g_ctx.swapchain_color_fmt = swapchain_color_format
        ? static_cast<VkFormat>(swapchain_color_format)
        : VK_FORMAT_B8G8R8A8_UNORM;
    impl::g_ctx.swapchain_depth_fmt = VK_FORMAT_D32_SFLOAT;
    impl::g_ctx.ready               = (impl::g_ctx.device != VK_NULL_HANDLE);
    if (impl::g_ctx.ready) {
        impl::drain_deferred_locked();
    }
}

#else // !PSYNDER_GX_BACKEND_VULKAN

// On non-Vulkan builds this TU compiles empty. The Metal TU
// (PipelineCreateMetal.mm) supplies the symbol implementations.

#endif // PSYNDER_GX_BACKEND_VULKAN
