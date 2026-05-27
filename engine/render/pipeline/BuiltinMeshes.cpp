// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/render/pipeline/BuiltinMeshes.cpp
//
// Lane 09 — procedural unit meshes (box / sphere / plane) the ECS extract
// draws scene props with. Built on the CPU and uploaded ONCE at create()
// (DESIGN §4.4 — no mid-frame GPU allocation). Each mesh is unit-sized so
// the entity's TransformWS world matrix (which bakes the authored scale)
// produces the correct world dimensions:
//   box    — corners at ±0.5 on each axis (half-extent 0.5)
//   sphere — radius 0.5, UV-sphere tessellation
//   plane  — ±0.5 quad in the XZ plane, normal +Y
//
// Vertex layout matches Pipeline_internal.h's Vertex (pos/normal/uv/color)
// and the opaque pass binds the index buffer as U16, so every mesh keeps
// its vertex count well under 65536.

#include "render/pipeline/Pipeline_internal.h"

#include "gpu/PublicGpu.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace psynder::render::pipeline {

namespace {

constexpr math::Vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

bool upload_mesh(Pipeline* p, BuiltinMesh& mesh,
                 const std::vector<Vertex>& verts,
                 const std::vector<std::uint16_t>& indices,
                 const char* vb_name, const char* ib_name) {
    const gpu::HeapKind heap = gpu::device_is_unified_memory(p->device)
                             ? gpu::HeapKind::DeviceLocal
                             : gpu::HeapKind::HostVisible;

    gpu::BufferDesc vb_desc{};
    vb_desc.size_bytes = verts.size() * sizeof(Vertex);
    vb_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Vertex);
    vb_desc.heap       = heap;
    vb_desc.debug_name = vb_name;
    mesh.vertex_buffer = gpu::create_buffer(p->device, vb_desc);
    if (!mesh.vertex_buffer) return false;
    if (void* mapped = gpu::buffer_map(mesh.vertex_buffer.get())) {
        std::memcpy(mapped, verts.data(), vb_desc.size_bytes);
        gpu::buffer_unmap(mesh.vertex_buffer.get());
    }
    mesh.vertex_count = static_cast<std::uint32_t>(verts.size());

    gpu::BufferDesc ib_desc{};
    ib_desc.size_bytes = indices.size() * sizeof(std::uint16_t);
    ib_desc.usage      = static_cast<std::uint32_t>(gpu::BufferUsage::Index);
    ib_desc.heap       = heap;
    ib_desc.debug_name = ib_name;
    mesh.index_buffer = gpu::create_buffer(p->device, ib_desc);
    if (!mesh.index_buffer) return false;
    if (void* mapped = gpu::buffer_map(mesh.index_buffer.get())) {
        std::memcpy(mapped, indices.data(), ib_desc.size_bytes);
        gpu::buffer_unmap(mesh.index_buffer.get());
    }
    mesh.index_count = static_cast<std::uint32_t>(indices.size());
    return true;
}

void build_box(std::vector<Vertex>& v, std::vector<std::uint16_t>& idx) {
    constexpr float h = 0.5f;
    struct Face { math::Vec3 n; math::Vec3 a, b, c, d; };
    const Face faces[6] = {
        {{ 0, 0, 1}, {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h}},  // +Z
        {{ 0, 0,-1}, { h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h}},  // -Z
        {{ 1, 0, 0}, { h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h}},  // +X
        {{-1, 0, 0}, {-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h}},  // -X
        {{ 0, 1, 0}, {-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h}},  // +Y
        {{ 0,-1, 0}, {-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h}},  // -Y
    };
    for (const Face& f : faces) {
        const auto base = static_cast<std::uint16_t>(v.size());
        v.push_back({f.a, f.n, {0.0f, 0.0f}, kWhite});
        v.push_back({f.b, f.n, {1.0f, 0.0f}, kWhite});
        v.push_back({f.c, f.n, {1.0f, 1.0f}, kWhite});
        v.push_back({f.d, f.n, {0.0f, 1.0f}, kWhite});
        idx.insert(idx.end(), {base, static_cast<std::uint16_t>(base + 1),
                               static_cast<std::uint16_t>(base + 2),
                               base, static_cast<std::uint16_t>(base + 2),
                               static_cast<std::uint16_t>(base + 3)});
    }
}

void build_sphere(std::vector<Vertex>& v, std::vector<std::uint16_t>& idx) {
    constexpr int kStacks = 12;
    constexpr int kSlices = 16;
    constexpr float kRadius = 0.5f;
    constexpr float kPi = 3.14159265358979323846f;
    for (int i = 0; i <= kStacks; ++i) {
        const float phi = kPi * static_cast<float>(i) / kStacks;  // 0..pi
        const float sp = std::sin(phi), cp = std::cos(phi);
        for (int j = 0; j <= kSlices; ++j) {
            const float theta = 2.0f * kPi * static_cast<float>(j) / kSlices;
            const float st = std::sin(theta), ct = std::cos(theta);
            const math::Vec3 n{sp * ct, cp, sp * st};
            const math::Vec3 pos{n.x * kRadius, n.y * kRadius, n.z * kRadius};
            const math::Vec2 uv{static_cast<float>(j) / kSlices,
                                static_cast<float>(i) / kStacks};
            v.push_back({pos, n, uv, kWhite});
        }
    }
    const int stride = kSlices + 1;
    for (int i = 0; i < kStacks; ++i) {
        for (int j = 0; j < kSlices; ++j) {
            const auto a = static_cast<std::uint16_t>(i * stride + j);
            const auto b = static_cast<std::uint16_t>((i + 1) * stride + j);
            const auto c = static_cast<std::uint16_t>((i + 1) * stride + j + 1);
            const auto d = static_cast<std::uint16_t>(i * stride + j + 1);
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    }
}

void build_plane(std::vector<Vertex>& v, std::vector<std::uint16_t>& idx) {
    constexpr float h = 0.5f;
    const math::Vec3 n{0.0f, 1.0f, 0.0f};
    v.push_back({{-h, 0.0f, -h}, n, {0.0f, 0.0f}, kWhite});
    v.push_back({{ h, 0.0f, -h}, n, {1.0f, 0.0f}, kWhite});
    v.push_back({{ h, 0.0f,  h}, n, {1.0f, 1.0f}, kWhite});
    v.push_back({{-h, 0.0f,  h}, n, {0.0f, 1.0f}, kWhite});
    idx.insert(idx.end(), {0, 1, 2, 0, 2, 3});
}

}  // namespace

bool upload_builtin_meshes(Pipeline* p) {
    if (!p || !p->device) {
        // Headless: leave the meshes empty. The extract still emits one
        // renderable per entity (with null buffers) so its transform/count
        // logic is testable without a GPU; the opaque pass skips null draws.
        return false;
    }

    std::vector<Vertex> verts;
    std::vector<std::uint16_t> idx;

    verts.clear(); idx.clear(); build_box(verts, idx);
    const bool ok_box = upload_mesh(p, p->builtin_meshes.box, verts, idx,
                                    "builtin_box_vb", "builtin_box_ib");
    verts.clear(); idx.clear(); build_sphere(verts, idx);
    const bool ok_sphere = upload_mesh(p, p->builtin_meshes.sphere, verts, idx,
                                       "builtin_sphere_vb", "builtin_sphere_ib");
    verts.clear(); idx.clear(); build_plane(verts, idx);
    const bool ok_plane = upload_mesh(p, p->builtin_meshes.plane, verts, idx,
                                      "builtin_plane_vb", "builtin_plane_ib");

    if (!ok_box || !ok_sphere || !ok_plane) {
        std::fputs("[psy::render::pipeline] upload_builtin_meshes: a mesh alloc failed\n",
                   stderr);
        return false;
    }
    std::printf("[psy::render::pipeline] builtin meshes uploaded "
                "(box=%u/%u sphere=%u/%u plane=%u/%u verts/idx)\n",
                p->builtin_meshes.box.vertex_count, p->builtin_meshes.box.index_count,
                p->builtin_meshes.sphere.vertex_count, p->builtin_meshes.sphere.index_count,
                p->builtin_meshes.plane.vertex_count, p->builtin_meshes.plane.index_count);
    return true;
}

void release_builtin_meshes(Pipeline* p) {
    if (!p) return;
    p->builtin_meshes = BuiltinMeshes{};
}

}  // namespace psynder::render::pipeline
