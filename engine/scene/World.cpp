// SPDX-License-Identifier: MIT
// Psynder -- archetype/chunk ECS storage for the public scene World API.

#include "World.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace psynder::scene {

namespace {

constexpr u32   kInvalidIndex = 0x00FF'FFFFu;
constexpr usize kTargetChunkBytes = 16 * 1024;
constexpr u32   kMaxChunkEntities = 1024;

struct ComponentRegistry {
    std::mutex                                         mutex;
    std::unordered_map<ComponentId, ComponentTypeInfo> types;  // keyed by stable id
};

ComponentRegistry& registry() {
    static ComponentRegistry r;
    return r;
}

usize align_up(usize value, usize align) noexcept {
    return (value + align - 1u) & ~(align - 1u);
}

u32 next_generation(u32 gen) noexcept {
    // 32-bit generation (matches Entity's high word). Skip 0 so a live entity's
    // packed handle is always non-zero (Entity::valid() == raw != 0).
    ++gen;
    return gen == 0u ? 1u : gen;
}

ComponentTypeInfo registered_type(ComponentId id) {
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    if (const auto it = r.types.find(id); it != r.types.end()) {
        return it->second;
    }
    return ComponentTypeInfo{ id, 0, 0, "" };
}

void ensure_registered_type(const ComponentTypeInfo& info) {
    if (info.id == 0u || info.size == 0u || info.align == 0u) return;
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    r.types.emplace(info.id, info);  // no-op if already registered
}

struct ComponentColumn {
    ComponentTypeInfo info{};
    usize             offset = 0;
};

struct Chunk {
    std::byte* bytes = nullptr;
    u32        count = 0;
    usize      align = alignof(std::max_align_t);

    Chunk() = default;
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    Chunk(Chunk&& other) noexcept
        : bytes(other.bytes), count(other.count), align(other.align) {
        other.bytes = nullptr;
        other.count = 0;
    }

    Chunk& operator=(Chunk&& other) noexcept {
        if (this == &other) return *this;
        release();
        bytes = other.bytes;
        count = other.count;
        align = other.align;
        other.bytes = nullptr;
        other.count = 0;
        return *this;
    }

    ~Chunk() { release(); }

    void release() noexcept {
        if (!bytes) return;
        ::operator delete(bytes, std::align_val_t{align});
        bytes = nullptr;
        count = 0;
    }
};

struct Archetype {
    std::vector<ComponentColumn> columns;
    std::vector<Chunk>           chunks;
    usize                        entity_offset = 0;
    usize                        chunk_bytes = 0;
    usize                        chunk_align = alignof(Entity);
    u32                          capacity = 1;

    bool has(ComponentId id) const noexcept {
        return std::any_of(columns.begin(), columns.end(), [id](const ComponentColumn& c) {
            return c.info.id == id;
        });
    }

    int column_index(ComponentId id) const noexcept {
        for (usize i = 0; i < columns.size(); ++i) {
            if (columns[i].info.id == id) return static_cast<int>(i);
        }
        return -1;
    }
};

struct EntityRecord {
    u32        generation = 1;
    bool       alive = false;
    Archetype* archetype = nullptr;
    u32        chunk = 0;
    u32        row = 0;
};

struct EntityLocation {
    u32 chunk = 0;
    u32 row = 0;
};

}  // namespace

// Per-World ECS storage. This is the pimpl target of World (declared in
// World.h). It lives at namespace scope (not in the anonymous namespace) so
// the type matches the header's forward declaration; it still freely uses the
// anonymous-namespace types above since this is a single translation unit.
struct WorldState {
    std::vector<EntityRecord>               entities;
    std::vector<u32>                        free_entities;
    std::vector<std::unique_ptr<Archetype>> archetypes;
    Archetype*                              empty_archetype = nullptr;
    // >0 while a for_each_chunk is in flight. Structural changes (create/
    // destroy/add/remove) hand out / invalidate raw chunk pointers, so they
    // must be deferred to a CommandBuffer during iteration. Debug-asserted.
    int                                     iteration_depth = 0;
};

namespace {

std::vector<ComponentId> key_for(const Archetype& archetype) {
    std::vector<ComponentId> key;
    key.reserve(archetype.columns.size());
    for (const ComponentColumn& column : archetype.columns) {
        key.push_back(column.info.id);
    }
    return key;
}

bool same_key(const Archetype& archetype, const std::vector<ComponentId>& key) {
    if (archetype.columns.size() != key.size()) return false;
    for (usize i = 0; i < key.size(); ++i) {
        if (archetype.columns[i].info.id != key[i]) return false;
    }
    return true;
}

void build_layout(Archetype& archetype) {
    usize row_bytes = sizeof(Entity);
    usize max_align = alignof(Entity);
    for (const ComponentColumn& column : archetype.columns) {
        row_bytes += column.info.size;
        max_align = std::max<usize>(max_align, column.info.align);
    }

    u32 capacity = static_cast<u32>(kTargetChunkBytes / std::max<usize>(row_bytes, 1u));
    capacity = std::clamp(capacity, 1u, kMaxChunkEntities);

    for (;;) {
        usize offset = 0;
        offset = align_up(offset, alignof(Entity));
        archetype.entity_offset = offset;
        offset += sizeof(Entity) * capacity;

        for (ComponentColumn& column : archetype.columns) {
            offset = align_up(offset, column.info.align);
            column.offset = offset;
            offset += static_cast<usize>(column.info.size) * capacity;
        }

        offset = align_up(offset, max_align);
        if (offset <= kTargetChunkBytes || capacity == 1u) {
            archetype.capacity = capacity;
            archetype.chunk_bytes = offset;
            archetype.chunk_align = max_align;
            return;
        }
        --capacity;
    }
}

Archetype* get_or_create_archetype(WorldState& s, const std::vector<ComponentId>& key) {
    for (const std::unique_ptr<Archetype>& archetype : s.archetypes) {
        if (same_key(*archetype, key)) return archetype.get();
    }

    auto archetype = std::make_unique<Archetype>();
    archetype->columns.reserve(key.size());
    for (const ComponentId id : key) {
        ComponentTypeInfo info = registered_type(id);
        if (info.id == 0u || info.size == 0u || info.align == 0u) {
            return nullptr;
        }
        archetype->columns.push_back(ComponentColumn{info, 0});
    }
    build_layout(*archetype);

    Archetype* raw = archetype.get();
    s.archetypes.push_back(std::move(archetype));
    if (key.empty()) s.empty_archetype = raw;
    return raw;
}

Archetype* empty_archetype(WorldState& s) {
    if (s.empty_archetype) return s.empty_archetype;
    const std::vector<ComponentId> key;
    return get_or_create_archetype(s, key);
}

Chunk* allocate_chunk(Archetype& archetype) {
    Chunk chunk;
    chunk.align = archetype.chunk_align;
    chunk.bytes = static_cast<std::byte*>(
        ::operator new(archetype.chunk_bytes, std::align_val_t{chunk.align}, std::nothrow));
    if (!chunk.bytes) return nullptr;
    std::memset(chunk.bytes, 0, archetype.chunk_bytes);

    archetype.chunks.push_back(std::move(chunk));
    return &archetype.chunks.back();
}

void* entity_column(const Archetype& archetype, Chunk& chunk) noexcept {
    return chunk.bytes + archetype.entity_offset;
}

void* component_column(const ComponentColumn& column, Chunk& chunk) noexcept {
    return chunk.bytes + column.offset;
}

Entity* entity_at(const Archetype& archetype, Chunk& chunk, u32 row) noexcept {
    return static_cast<Entity*>(entity_column(archetype, chunk)) + row;
}

void* component_at(const ComponentColumn& column, Chunk& chunk, u32 row) noexcept {
    return static_cast<std::byte*>(component_column(column, chunk)) +
           static_cast<usize>(column.info.size) * row;
}

EntityLocation append_entity(Archetype& archetype, Entity entity) {
    u32 chunk_index = 0;
    for (; chunk_index < archetype.chunks.size(); ++chunk_index) {
        if (archetype.chunks[chunk_index].count < archetype.capacity) break;
    }
    if (chunk_index == archetype.chunks.size() && !allocate_chunk(archetype)) {
        return EntityLocation{kInvalidIndex, kInvalidIndex};
    }

    Chunk& chunk = archetype.chunks[chunk_index];
    const u32 row = chunk.count++;
    *entity_at(archetype, chunk, row) = entity;
    return EntityLocation{chunk_index, row};
}

void erase_entity_at(WorldState& s, Archetype& archetype, u32 chunk_index, u32 row) {
    if (chunk_index >= archetype.chunks.size()) return;
    Chunk& chunk = archetype.chunks[chunk_index];
    if (row >= chunk.count) return;

    const u32 last_row = chunk.count - 1u;
    if (row != last_row) {
        const Entity moved = *entity_at(archetype, chunk, last_row);
        *entity_at(archetype, chunk, row) = moved;
        for (const ComponentColumn& column : archetype.columns) {
            std::memcpy(component_at(column, chunk, row),
                        component_at(column, chunk, last_row),
                        column.info.size);
        }

        const u32 moved_index = moved.index();
        if (moved_index < s.entities.size()) {
            EntityRecord& record = s.entities[moved_index];
            record.chunk = chunk_index;
            record.row = row;
        }
    }

    --chunk.count;
}

bool valid_entity(WorldState& s, Entity e, EntityRecord*& out) noexcept {
    const u32 index = e.index();
    if (!e.valid() || index >= s.entities.size()) return false;

    EntityRecord& record = s.entities[index];
    if (!record.alive || record.generation != e.gen()) return false;
    out = &record;
    return true;
}

std::vector<ComponentId> key_with_added(const Archetype& archetype, ComponentId id) {
    std::vector<ComponentId> key = key_for(archetype);
    key.push_back(id);
    std::sort(key.begin(), key.end());
    key.erase(std::unique(key.begin(), key.end()), key.end());
    return key;
}

std::vector<ComponentId> key_without(const Archetype& archetype, ComponentId id) {
    std::vector<ComponentId> key;
    key.reserve(archetype.columns.size());
    for (const ComponentColumn& column : archetype.columns) {
        if (column.info.id != id) key.push_back(column.info.id);
    }
    return key;
}

bool migrate(WorldState& s, Entity e, EntityRecord& record, Archetype& dst,
             const ComponentTypeInfo* added_info, const void* added_component,
             ComponentId removed_id) {
    Archetype* src = record.archetype;
    if (!src) return false;

    EntityLocation location = append_entity(dst, e);
    if (location.chunk == kInvalidIndex) return false;

    Chunk& src_chunk = src->chunks[record.chunk];
    Chunk& dst_chunk = dst.chunks[location.chunk];

    for (const ComponentColumn& dst_column : dst.columns) {
        void* dst_ptr = component_at(dst_column, dst_chunk, location.row);
        if (added_info && dst_column.info.id == added_info->id) {
            std::memcpy(dst_ptr, added_component, added_info->size);
            continue;
        }
        if (dst_column.info.id == removed_id) continue;

        const int src_column_index = src->column_index(dst_column.info.id);
        if (src_column_index >= 0) {
            const ComponentColumn& src_column = src->columns[static_cast<usize>(src_column_index)];
            std::memcpy(dst_ptr, component_at(src_column, src_chunk, record.row), src_column.info.size);
        }
    }

    const u32 old_chunk = record.chunk;
    const u32 old_row = record.row;
    erase_entity_at(s, *src, old_chunk, old_row);
    record.archetype = &dst;
    record.chunk = location.chunk;
    record.row = location.row;
    return true;
}

}  // namespace

ComponentId register_component(const ComponentTypeInfo& info) {
    // `info.id` is the stable name-hash computed by component_id<T>(); it is the
    // sole registrar. Idempotent per type; a clash between two *distinct* names
    // hashing to the same id is a (vanishingly unlikely) FNV collision and is a
    // hard error in debug so it is caught at bring-up rather than corrupting
    // archetype layout.
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    const auto [it, inserted] = r.types.emplace(info.id, info);
    if (!inserted && it->second.name != nullptr && info.name != nullptr) {
        assert(std::strcmp(it->second.name, info.name) == 0 &&
               "component id hash collision between two distinct component names");
    }
    return info.id;
}

World::World() : state_(std::make_unique<WorldState>()) {}
World::~World() = default;

World& World::Get() {
    static World w;
    return w;
}

// Asserts no for_each_chunk is in flight: a structural change reallocates/
// migrates chunk storage and would dangle the raw pointers a query handed out.
// Defer such changes to a CommandBuffer and play them back at a sync point.
#define PSY_WORLD_NO_ITERATION(s) \
    assert((s).iteration_depth == 0 && \
           "structural change during for_each_chunk; defer via CommandBuffer")

Entity World::create() {
    WorldState& s = *state_;
    PSY_WORLD_NO_ITERATION(s);
    Archetype* archetype = empty_archetype(s);
    if (!archetype) return {};

    u32 index = 0;
    if (!s.free_entities.empty()) {
        index = s.free_entities.back();
        s.free_entities.pop_back();
    } else {
        if (s.entities.size() > kInvalidIndex) return {};
        index = static_cast<u32>(s.entities.size());
        s.entities.push_back(EntityRecord{});
    }

    EntityRecord& record = s.entities[index];
    record.alive = true;
    record.archetype = archetype;
    const Entity e{(static_cast<u64>(record.generation) << 32) | static_cast<u64>(index)};

    const EntityLocation location = append_entity(*archetype, e);
    if (location.chunk == kInvalidIndex) {
        record.alive = false;
        record.archetype = nullptr;
        s.free_entities.push_back(index);
        return {};
    }

    record.chunk = location.chunk;
    record.row = location.row;
    return e;
}

void World::destroy(Entity e) {
    WorldState& s = *state_;
    PSY_WORLD_NO_ITERATION(s);
    EntityRecord* record = nullptr;
    if (!valid_entity(s, e, record)) return;

    Archetype* archetype = record->archetype;
    const u32 chunk = record->chunk;
    const u32 row = record->row;
    erase_entity_at(s, *archetype, chunk, row);

    record->alive = false;
    record->archetype = nullptr;
    record->chunk = 0;
    record->row = 0;
    record->generation = next_generation(record->generation);
    s.free_entities.push_back(e.index());
}

bool World::alive(Entity e) const noexcept {
    EntityRecord* record = nullptr;
    return valid_entity(*state_, e, record);
}

void World::add_component(Entity e, const ComponentTypeInfo& info, const void* component) {
    if (!component || info.id == 0u || info.size == 0u || info.align == 0u) return;
    ensure_registered_type(info);

    WorldState& s = *state_;
    PSY_WORLD_NO_ITERATION(s);
    EntityRecord* record = nullptr;
    if (!valid_entity(s, e, record) || !record->archetype) return;

    Archetype& src = *record->archetype;
    if (const int existing = src.column_index(info.id); existing >= 0) {
        Chunk& chunk = src.chunks[record->chunk];
        const ComponentColumn& column = src.columns[static_cast<usize>(existing)];
        std::memcpy(component_at(column, chunk, record->row), component, column.info.size);
        return;
    }

    Archetype* dst = get_or_create_archetype(s, key_with_added(src, info.id));
    if (!dst) return;
    (void)migrate(s, e, *record, *dst, &info, component, 0u);
}

void* World::get_component(Entity e, ComponentId id) noexcept {
    EntityRecord* record = nullptr;
    if (!valid_entity(*state_, e, record) || !record->archetype) return nullptr;

    Archetype& archetype = *record->archetype;
    const int column_index = archetype.column_index(id);
    if (column_index < 0) return nullptr;

    Chunk& chunk = archetype.chunks[record->chunk];
    const ComponentColumn& column = archetype.columns[static_cast<usize>(column_index)];
    return component_at(column, chunk, record->row);
}

void World::remove_component(Entity e, ComponentId id) {
    WorldState& s = *state_;
    PSY_WORLD_NO_ITERATION(s);
    EntityRecord* record = nullptr;
    if (!valid_entity(s, e, record) || !record->archetype) return;

    Archetype& src = *record->archetype;
    if (!src.has(id)) return;

    Archetype* dst = get_or_create_archetype(s, key_without(src, id));
    if (!dst) return;
    (void)migrate(s, e, *record, *dst, nullptr, nullptr, id);
}

void World::query_chunks(const ComponentId* ids, usize id_count,
                         void* ctx, void (*fn)(void*, const ChunkView&)) {
    if (!fn) return;
    // Stack-only working set — the query path never allocates.
    constexpr usize kMaxQueryComponents = 16;
    if (id_count > kMaxQueryComponents) return;

    WorldState& s = *state_;
    ++s.iteration_depth;  // forbids structural changes until this query returns
    for (const std::unique_ptr<Archetype>& archetype_ptr : s.archetypes) {
        Archetype& archetype = *archetype_ptr;

        // Resolve every requested component to a column index in this
        // archetype once, up front; skip archetypes missing any of them.
        int column_index[kMaxQueryComponents];
        bool matches = true;
        for (usize i = 0; i < id_count; ++i) {
            const int ci = archetype.column_index(ids[i]);
            if (ci < 0) {
                matches = false;
                break;
            }
            column_index[i] = ci;
        }
        if (!matches) continue;

        for (Chunk& chunk : archetype.chunks) {
            if (chunk.count == 0) continue;

            void* columns[kMaxQueryComponents];
            for (usize i = 0; i < id_count; ++i) {
                columns[i] = component_column(
                    archetype.columns[static_cast<usize>(column_index[i])], chunk);
            }

            ChunkView view{};
            view.count    = chunk.count;
            view.entities = static_cast<const Entity*>(entity_column(archetype, chunk));
            view.columns  = columns;
            fn(ctx, view);
        }
    }
    --s.iteration_depth;
}

}  // namespace psynder::scene
