// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/scene/SceneSerialize.cpp — see SceneSerialize.h for the byte format,
// component set, and determinism notes.

#include "scene/SceneSerialize.h"

#include "scene/GxComponents.h"     // TransformWS
#include "scene/SceneComponents.h"  // Collider, RenderMaterial, DynamicBody, ShapeKind
#include "scene/World.h"

#include <bit>
#include <cstring>

namespace psynder::scene {

namespace {

// ─── Little-endian byte writers (append) ──────────────────────────────────
// Shift each byte out individually so the stream is bit-identical regardless of
// host endianness — never memcpy an integer (that would be host-order).
inline void put_u32_le(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

// Write an f32 by its exact IEEE-754 bit pattern, so the value round-trips
// bit-for-bit (no rounding, no host float-format assumption beyond IEEE-754,
// which Types.h already static_asserts via sizeof checks).
inline void put_f32_le(std::vector<u8>& out, f32 v) {
    put_u32_le(out, std::bit_cast<u32>(v));
}

// ─── Bounded little-endian byte readers ───────────────────────────────────
// `off` advances on success. Each read first checks that the field fits inside
// `bytes`; on overrun it returns false and leaves `off` unspecified — the
// caller aborts the whole decode.
inline bool read_u32_le(std::span<const u8> bytes, usize& off, u32& v) noexcept {
    if (off + sizeof(u32) > bytes.size()) return false;
    const u8* p = bytes.data() + off;
    v = static_cast<u32>(p[0]) |
        (static_cast<u32>(p[1]) << 8) |
        (static_cast<u32>(p[2]) << 16) |
        (static_cast<u32>(p[3]) << 24);
    off += sizeof(u32);
    return true;
}

inline bool read_f32_le(std::span<const u8> bytes, usize& off, f32& v) noexcept {
    u32 bits = 0;
    if (!read_u32_le(bytes, off, bits)) return false;
    v = std::bit_cast<f32>(bits);
    return true;
}

// ─── Per-component payload writers ─────────────────────────────────────────
inline void write_transform(std::vector<u8>& out, const TransformWS& t) {
    for (int i = 0; i < 16; ++i) put_f32_le(out, t.mtw.m[i]);
    for (int i = 0; i < 16; ++i) put_f32_le(out, t.prev_mtw.m[i]);
}

inline void write_collider(std::vector<u8>& out, const Collider& c) {
    put_u32_le(out, static_cast<u32>(c.kind));
    put_f32_le(out, c.half_extents.x);
    put_f32_le(out, c.half_extents.y);
    put_f32_le(out, c.half_extents.z);
}

inline void write_material(std::vector<u8>& out, const RenderMaterial& m) {
    put_f32_le(out, m.albedo.x);
    put_f32_le(out, m.albedo.y);
    put_f32_le(out, m.albedo.z);
    put_f32_le(out, m.roughness);
    put_f32_le(out, m.metallic);
    put_f32_le(out, m.emissive.x);
    put_f32_le(out, m.emissive.y);
    put_f32_le(out, m.emissive.z);
    put_f32_le(out, m.emissive_intensity);
}

inline void write_dynamic_body(std::vector<u8>& out, const DynamicBody& d) {
    put_f32_le(out, d.mass_kg);
    put_f32_le(out, d.friction);
    put_f32_le(out, d.restitution);
}

// ─── Per-component payload readers ─────────────────────────────────────────
inline bool read_transform(std::span<const u8> bytes, usize& off, TransformWS& t) noexcept {
    for (int i = 0; i < 16; ++i)
        if (!read_f32_le(bytes, off, t.mtw.m[i])) return false;
    for (int i = 0; i < 16; ++i)
        if (!read_f32_le(bytes, off, t.prev_mtw.m[i])) return false;
    return true;
}

inline bool read_collider(std::span<const u8> bytes, usize& off, Collider& c) noexcept {
    u32 kind = 0;
    if (!read_u32_le(bytes, off, kind)) return false;
    c.kind = static_cast<ShapeKind>(kind);
    return read_f32_le(bytes, off, c.half_extents.x) &&
           read_f32_le(bytes, off, c.half_extents.y) &&
           read_f32_le(bytes, off, c.half_extents.z);
}

inline bool read_material(std::span<const u8> bytes, usize& off, RenderMaterial& m) noexcept {
    return read_f32_le(bytes, off, m.albedo.x) &&
           read_f32_le(bytes, off, m.albedo.y) &&
           read_f32_le(bytes, off, m.albedo.z) &&
           read_f32_le(bytes, off, m.roughness) &&
           read_f32_le(bytes, off, m.metallic) &&
           read_f32_le(bytes, off, m.emissive.x) &&
           read_f32_le(bytes, off, m.emissive.y) &&
           read_f32_le(bytes, off, m.emissive.z) &&
           read_f32_le(bytes, off, m.emissive_intensity);
}

inline bool read_dynamic_body(std::span<const u8> bytes, usize& off, DynamicBody& d) noexcept {
    return read_f32_le(bytes, off, d.mass_kg) &&
           read_f32_le(bytes, off, d.friction) &&
           read_f32_le(bytes, off, d.restitution);
}

// All mask bits this build understands. Any bit outside this set in a decoded
// mask means a newer/corrupt stream we cannot safely advance past.
inline constexpr u32 kKnownMask =
    kBitTransformWS | kBitCollider | kBitRenderMaterial | kBitDynamicBody;

}  // namespace

void serialize_scene(World& w, std::vector<u8>& out) {
    out.clear();

    // Two-pass: first count the TransformWS-bearing entities so the header can
    // carry an exact count; then emit records. Counting and emitting both walk
    // chunks in the same archetype/chunk order, so the count matches the body.
    u32 entity_count = 0;
    w.for_each_chunk<TransformWS>([&](usize n, TransformWS*) {
        entity_count += static_cast<u32>(n);
    });

    put_u32_le(out, kSceneMagic);
    put_u32_le(out, kSceneVersion);
    put_u32_le(out, entity_count);

    // Body pass. We iterate the TransformWS query (the anchor every serialized
    // entity has) and, per entity, probe the optional components by handle so a
    // single stable order is used regardless of which archetype an entity sits
    // in. get<T> is cheap and avoids needing a separate query per component
    // combination.
    w.for_each_chunk_with_entities<TransformWS>(
        [&](usize n, const Entity* entities, TransformWS* xforms) {
            for (usize i = 0; i < n; ++i) {
                const Entity e = entities[i];

                const Collider*       col  = w.get<Collider>(e);
                const RenderMaterial* mat  = w.get<RenderMaterial>(e);
                const DynamicBody*    body = w.get<DynamicBody>(e);

                u32 mask = kBitTransformWS;
                if (col)  mask |= kBitCollider;
                if (mat)  mask |= kBitRenderMaterial;
                if (body) mask |= kBitDynamicBody;
                put_u32_le(out, mask);

                write_transform(out, xforms[i]);
                if (col)  write_collider(out, *col);
                if (mat)  write_material(out, *mat);
                if (body) write_dynamic_body(out, *body);
            }
        });
}

bool deserialize_scene(std::span<const u8> bytes, World& out) {
    usize off = 0;

    u32 magic = 0;
    u32 version = 0;
    u32 entity_count = 0;
    if (!read_u32_le(bytes, off, magic))        return false;
    if (!read_u32_le(bytes, off, version))      return false;
    if (!read_u32_le(bytes, off, entity_count)) return false;
    if (magic != kSceneMagic)     return false;
    if (version != kSceneVersion) return false;

    for (u32 i = 0; i < entity_count; ++i) {
        u32 mask = 0;
        if (!read_u32_le(bytes, off, mask)) return false;
        // Reject masks with unknown bits or no TransformWS anchor.
        if ((mask & ~kKnownMask) != 0u)         return false;
        if ((mask & kBitTransformWS) == 0u)     return false;

        TransformWS    xform{};
        Collider       col{};
        RenderMaterial mat{};
        DynamicBody    body{};

        if (!read_transform(bytes, off, xform)) return false;
        if (mask & kBitCollider)
            if (!read_collider(bytes, off, col)) return false;
        if (mask & kBitRenderMaterial)
            if (!read_material(bytes, off, mat)) return false;
        if (mask & kBitDynamicBody)
            if (!read_dynamic_body(bytes, off, body)) return false;

        // Only mutate the World once the full record has been validated, so a
        // truncated tail does not leave a half-populated entity.
        const Entity e = out.create();
        out.add(e, xform);
        if (mask & kBitCollider)       out.add(e, col);
        if (mask & kBitRenderMaterial) out.add(e, mat);
        if (mask & kBitDynamicBody)    out.add(e, body);
    }

    return true;
}

}  // namespace psynder::scene
