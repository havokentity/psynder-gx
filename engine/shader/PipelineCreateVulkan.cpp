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
// Push-constant ABI: a single 128-byte (kMaxPushConstantBytes) range at
// offset 0. Graphics layouts declare the range over VERTEX|FRAGMENT — exactly
// the stages lane 07's push_constants() encoder emits (ShaderStage::AllGfx, and
// its unknown-mask fallback). vkCmdPushConstants checks both directions, so the
// layout range and the pushed stageFlags must match exactly
// (VUID-vkCmdPushConstants-offset-01795/01796). Compute layouts span COMPUTE only.
//
// Descriptor sets (M1): graphics layouts attach lane 07's set-0 layout
// (sampled image + sampler) via psynder_gx_vk_m1_texture_set_layout() so the
// textured-triangle shader resolves. Compute uses no descriptor sets yet.

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

// Lane 07 owns the M1 textured-triangle descriptor-set-0 layout (binding 0 =
// sampled image, binding 1 = sampler). It builds it before pushing the VkDevice
// context, so this returns a live VkDescriptorSetLayout by the time we drain the
// deferred-gfx queue. Returns null pre-handshake or if infra creation failed; in
// that case graphics PSOs fall back to a no-descriptor-set layout. Identically
// defined set layouts are Vulkan-compatible, so a set lane 07 allocates against
// its copy binds cleanly to the layout we build here.
extern "C" void* psynder_gx_vk_m1_texture_set_layout();

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
    std::uint32_t              color_format_count = 1;
    std::uint8_t               depth_format = 0;
    bool                       enable_depth_write = true;
    bool                       enable_blend = false;
    // Wireframe polygon fill (GraphicsPipelineDesc::fill_mode == Wireframe).
    // Maps to VK_POLYGON_MODE_LINE when fillModeNonSolid is supported, else
    // falls back to VK_POLYGON_MODE_FILL (logged once).
    bool                       fill_wireframe = false;
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

VkFormat to_vk_depth_format(std::uint8_t format) {
    // Mirrors gpu::Format without depending on lane 07 headers.
    constexpr std::uint8_t kDepth32Float = 8;
    constexpr std::uint8_t kDepth24UnormStencil8 = 9;
    switch (format) {
        case 0: return VK_FORMAT_UNDEFINED;
        case kDepth32Float: return VK_FORMAT_D32_SFLOAT;
        case kDepth24UnormStencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
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

// Build a pipeline layout: one 128-byte push-constant range over `push_stages`
// plus an optional descriptor set 0. `set0 == VK_NULL_HANDLE` ⇒ no descriptor
// sets (compute, or a graphics shader that samples nothing). `push_stages` must
// EXACTLY equal the stages lane 07's encoder pushes (vkCmdPushConstants checks
// both directions): graphics passes VERTEX|FRAGMENT (= ShaderStage::AllGfx),
// compute passes COMPUTE only.
VkPipelineLayout make_pipeline_layout(VkShaderStageFlags    push_stages,
                                      VkDescriptorSetLayout set0) {
    VkPushConstantRange pcr{};
    pcr.stageFlags = push_stages;
    pcr.offset     = 0;
    pcr.size       = 128;

    VkDescriptorSetLayout set_layouts[1] = { set0 };
    VkPipelineLayoutCreateInfo lci{};
    lci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount         = (set0 != VK_NULL_HANDLE) ? 1u : 0u;
    lci.pSetLayouts            = (set0 != VK_NULL_HANDLE) ? set_layouts : nullptr;
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

    // M1: attach descriptor set 0 (lane 07's sampled-image + sampler layout)
    // so the textured-triangle fragment shader's set-0 bindings resolve. A
    // graphics shader that samples nothing simply leaves the set unused — a
    // pipeline layout may declare descriptor sets the shader doesn't consume.
    auto set0 = reinterpret_cast<VkDescriptorSetLayout>(
        psynder_gx_vk_m1_texture_set_layout());
    // VERTEX|FRAGMENT — must EXACTLY equal the stages lane 07's encoder pushes
    // (ShaderStage::AllGfx = Vertex|Fragment). vkCmdPushConstants requires the
    // pushed stageFlags to cover every stage in each overlapping range, so a
    // broader ALL_GRAPHICS range here would reject a VERTEX|FRAGMENT push
    // (VUID-vkCmdPushConstants-offset-01796). M1 graphics PSOs only have
    // vertex+fragment stages, so this is also the complete set.
    VkPipelineLayout layout = make_pipeline_layout(
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, set0);
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
            // effective_binding_stride() resolves a 0 stride to the tightly-
            // packed implicit stride; reading .stride directly would leave a
            // stride-0 binding (invalid for an attributed binding).
            vk_bindings[b].stride    = effective_binding_stride(d.vi, b);
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
    // lineWidth stays 1.0 — wider strokes need the wideLines feature which
    // we deliberately do not require. VK_POLYGON_MODE_LINE only needs the
    // fillModeNonSolid feature, which VulkanBackend.cpp enables when the
    // GPU advertises it. We query the SAME physical-device feature here so
    // the polygon mode matches what the device was actually created with;
    // when unsupported we fall back to FILL (logged once, no crash).
    rs.lineWidth    = 1.0f;
    if (d.fill_wireframe) {
        VkPhysicalDeviceFeatures feats{};
        if (g_ctx.phys != VK_NULL_HANDLE) {
            vkGetPhysicalDeviceFeatures(g_ctx.phys, &feats);
        }
        if (feats.fillModeNonSolid) {
            rs.polygonMode = VK_POLYGON_MODE_LINE;
        } else {
            static bool s_warned = false;
            if (!s_warned) {
                std::fprintf(stderr,
                    "[psy::shader::vk] fillModeNonSolid unsupported — wireframe "
                    "pipeline (id=%u) falls back to solid fill\n", d.id);
                s_warned = true;
            }
        }
    }

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const VkFormat depth_format = to_vk_depth_format(d.depth_format);
    if (d.depth_format != 0 && depth_format == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr,
            "[psy::shader::vk] unsupported depth_format=%u for id=%u\n",
            static_cast<unsigned>(d.depth_format), d.id);
        vkDestroyShaderModule(g_ctx.device, vs_mod, nullptr);
        vkDestroyShaderModule(g_ctx.device, fs_mod, nullptr);
        vkDestroyPipelineLayout(g_ctx.device, layout, nullptr);
        return false;
    }
    const bool has_depth = depth_format != VK_FORMAT_UNDEFINED;

    VkPipelineDepthStencilStateCreateInfo dss{};
    dss.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable  = has_depth ? VK_TRUE : VK_FALSE;
    dss.depthWriteEnable = (has_depth && d.enable_depth_write) ? VK_TRUE : VK_FALSE;
    dss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    const std::uint32_t color_count = d.color_format_count == 0 ? 0u : 1u;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    att.blendEnable = d.enable_blend ? VK_TRUE : VK_FALSE;
    if (d.enable_blend) {
        att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = color_count;
    cbs.pAttachments    = color_count ? &att : nullptr;

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
    prc.colorAttachmentCount    = color_count;
    prc.pColorAttachmentFormats = color_count ? color_fmts : nullptr;
    prc.depthAttachmentFormat   = depth_format;

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

    // Compute: COMPUTE-only push range, no descriptor sets in M1.
    VkPipelineLayout layout =
        make_pipeline_layout(VK_SHADER_STAGE_COMPUTE_BIT, VK_NULL_HANDLE);
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
    const VertexInputDesc&            vertex_input,
    std::uint32_t                     color_format_count,
    std::uint8_t                      depth_format,
    bool                              enable_depth_write,
    bool                              enable_blend,
    bool                              fill_wireframe)
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
        d.color_format_count = color_format_count;
        d.depth_format = depth_format;
        d.enable_depth_write = enable_depth_write;
        d.enable_blend = enable_blend;
        d.fill_wireframe = fill_wireframe;
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
    d.color_format_count = color_format_count;
    d.depth_format = depth_format;
    d.enable_depth_write = enable_depth_write;
    d.enable_blend = enable_blend;
    d.fill_wireframe = fill_wireframe;
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
