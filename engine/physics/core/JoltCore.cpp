// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/JoltCore.cpp
//
// Lane 15 — Jolt Physics wrapper.
//
// Implements every symbol in PublicPhysicsCore.h on top of Jolt's
// PhysicsSystem / BodyInterface / CharacterVirtual. The contract is
// frozen (DESIGN §10.1); this file is the only place Jolt types are
// allowed to escape into our codebase, so downstream lanes never need
// to include Jolt headers and stay independent of the upstream tree.
//
// Determinism: this TU is compiled with -fno-fast-math + -ffp-contract=off
// (see CMakeLists.txt), and Jolt itself is built with
// CROSS_PLATFORM_DETERMINISTIC=ON. Combined, ticks are bitwise
// reproducible across arm64 and x86_64 — required for lockstep replay
// and netcode rewind (DESIGN §10.1, §10.4).
//
// Threading: World::tick currently calls PhysicsSystem::Update from the
// caller thread with a single-thread JobSystem. This trades a small
// amount of throughput for the simplest deterministic model. Lane 04
// (jobs) will wire a multithreaded JobSystem adapter in a later wave;
// that change is local to this file and does not touch the public API.

#include "physics/core/PublicPhysicsCore.h"

// Jolt's NEON code path in DVec3.inl trips -Wdouble-promotion (it
// passes float literals to f64 intrinsics intentionally). The project's
// warning policy enables -Wdouble-promotion via psynder_common INTERFACE,
// which gets appended after our PRIVATE compile options, so we silence
// the diagnostic right around the Jolt headers instead of via CMake.
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdouble-promotion"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif

#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

JPH_SUPPRESS_WARNINGS

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <new>

namespace {

// ─── Jolt one-time init / shutdown ────────────────────────────────────────
//
// Jolt requires RegisterDefaultAllocator + Factory + RegisterTypes once
// per process before any PhysicsSystem is constructed. Use an atomic
// counter so the first World::create_world performs init and the last
// destroy_world tears it down. Tests and samples that churn worlds keep
// working without leaking factory state.

std::atomic<std::uint32_t> g_jolt_init_refcount{0};

void TraceImpl(const char* fmt, ...) {
    // Minimal trace sink — wire to psy::log later. Avoid pulling Log.h
    // here to keep this TU's link surface tight. The non-literal format
    // string is intentional (Jolt owns the format), so suppress the
    // diagnostic only around the variadic forward.
    char buf[1024];
    std::va_list args;
    va_start(args, fmt);
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wformat-nonliteral"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    std::vsnprintf(buf, sizeof(buf), fmt, args);
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
    va_end(args);
    std::fputs("[jolt] ", stderr);
    std::fputs(buf, stderr);
    std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool AssertFailedImpl(const char* expr, const char* msg, const char* file, JPH::uint line) {
    std::fprintf(stderr, "[jolt] assert: %s:%u (%s) %s\n",
                 file, line, expr, msg ? msg : "");
    return true; // breakpoint
}
#endif

void JoltGlobalInit() {
    if (g_jolt_init_refcount.fetch_add(1, std::memory_order_acq_rel) == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = TraceImpl;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void JoltGlobalShutdown() {
    if (g_jolt_init_refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

// ─── Layer mapping ────────────────────────────────────────────────────────
//
// Two-layer scheme is enough for M4: NON_MOVING (static world geometry)
// and MOVING (dynamic bodies + characters). More fine-grained layers
// (TEAM_A, TEAM_B, RAGDOLL, DEBRIS, ...) get added in later waves
// without breaking the PublicPhysicsCore.h surface.

namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING     = 1;
constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0);
constexpr JPH::BroadPhaseLayer MOVING(1);
constexpr JPH::uint NUM_LAYERS(2);
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        mMap[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mMap[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::NUM_LAYERS;
    }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return mMap[l];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        switch (static_cast<JPH::BroadPhaseLayer::Type>(l)) {
            case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::NON_MOVING):
                return "NON_MOVING";
            case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::MOVING):
                return "MOVING";
            default: return "INVALID";
        }
    }
#endif
private:
    JPH::BroadPhaseLayer mMap[Layers::NUM_LAYERS];
};

class ObjectVsBPLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override {
        switch (obj) {
            case Layers::NON_MOVING: return bp == BroadPhaseLayers::MOVING;
            case Layers::MOVING:     return true;
            default: return false;
        }
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        switch (a) {
            case Layers::NON_MOVING: return b == Layers::MOVING;
            case Layers::MOVING:     return true;
            default: return false;
        }
    }
};

// ─── Helpers ──────────────────────────────────────────────────────────────
inline JPH::RVec3 R3(const float p[3]) {
    return JPH::RVec3(static_cast<JPH::Real>(p[0]),
                      static_cast<JPH::Real>(p[1]),
                      static_cast<JPH::Real>(p[2]));
}
inline JPH::Quat Q(const float q[4]) {
    JPH::Quat out(q[0], q[1], q[2], q[3]);
    // Defend against the all-zero quaternion the default-constructed
    // BodyDesc{} produces — Jolt asserts on a non-unit rotation.
    if (out.LengthSq() < 1.0e-12f) {
        return JPH::Quat::sIdentity();
    }
    return out.Normalized();
}
inline void WriteV3(JPH::RVec3Arg p, float out[3]) {
    out[0] = static_cast<float>(p.GetX());
    out[1] = static_cast<float>(p.GetY());
    out[2] = static_cast<float>(p.GetZ());
}
inline void WriteQ(JPH::QuatArg q, float out[4]) {
    out[0] = q.GetX();
    out[1] = q.GetY();
    out[2] = q.GetZ();
    out[3] = q.GetW();
}

// Build a Jolt Shape from BodyDesc. Returns a ref-counted shape on
// success, nullptr if the description is unsupported (e.g. mesh data
// not yet wired in — that comes with the asset pipeline in M2/M3).
JPH::ShapeRefC MakeShape(const psynder::physics::BodyDesc& d) {
    using namespace psynder::physics;
    switch (d.shape) {
        case Shape::Box: {
            // dims[0..2] = half-extents in metres.
            const float hx = d.dims[0] > 0.0f ? d.dims[0] : 0.5f;
            const float hy = d.dims[1] > 0.0f ? d.dims[1] : 0.5f;
            const float hz = d.dims[2] > 0.0f ? d.dims[2] : 0.5f;
            JPH::BoxShapeSettings s(JPH::Vec3(hx, hy, hz));
            s.SetEmbedded();
            JPH::ShapeSettings::ShapeResult r = s.Create();
            return r.IsValid() ? r.Get() : JPH::ShapeRefC();
        }
        case Shape::Sphere: {
            const float radius = d.dims[0] > 0.0f ? d.dims[0] : 0.5f;
            JPH::SphereShapeSettings s(radius);
            s.SetEmbedded();
            JPH::ShapeSettings::ShapeResult r = s.Create();
            return r.IsValid() ? r.Get() : JPH::ShapeRefC();
        }
        case Shape::Capsule: {
            // dims[0] = radius, dims[1] = half-height of the cylindrical
            // segment (Jolt convention; total capsule height = 2*(radius
            // + half_height)).
            const float radius      = d.dims[0] > 0.0f ? d.dims[0] : 0.4f;
            const float half_height = d.dims[1] > 0.0f ? d.dims[1] : 0.9f;
            JPH::CapsuleShapeSettings s(half_height, radius);
            s.SetEmbedded();
            JPH::ShapeSettings::ShapeResult r = s.Create();
            return r.IsValid() ? r.Get() : JPH::ShapeRefC();
        }
        case Shape::ConvexHull:   // Wired when the asset pipeline ships
        case Shape::TriangleMesh: // mesh / hull data (M2 / M3, lane 05).
        default:
            return JPH::ShapeRefC();
    }
}

} // anonymous namespace

// ─── psy::physics opaque types ───────────────────────────────────────────
//
// PublicPhysicsCore.h forward-declares World / RigidBody /
// CharacterController as `struct` types; we *define* those structs here
// so downstream code only ever sees opaque pointers. Linkage works
// because the public header treats them as incomplete types.

namespace psynder::physics {

// Compute the TempAllocator size from the body count.
//
// Derivation (Option A):
//   Jolt's documented per-body overhead during a single physics step is
//   roughly 640 B/body for contact manifolds + broad-phase scratch +
//   island builder work.  We round up to 1 KiB/body to give a comfortable
//   safety margin and account for constraint-side scratch (the two default
//   to the same count).  A 16 MiB floor keeps tiny test worlds (a few dozen
//   bodies) from over-allocating.
//
//   Reference: https://jrouwe.github.io/JoltPhysics/ — "Memory Usage" note
//   in PhysicsSystem::Update docs (~640 B/active body at peak step).
//
// For the default WorldDesc (max_bodies = 64 000):
//   max(16 MiB, 64 000 * 1 KiB) = max(16 777 216, 65 536 000) ≈ 62.5 MiB.
//   This comfortably covers the measured ~55 MiB peak on a full 64k-body tick.
inline std::size_t TempAllocSize(std::uint32_t max_bodies) noexcept {
    constexpr std::size_t kFloor       = 16u * 1024u * 1024u;   // 16 MiB
    constexpr std::size_t kBytesPerBody = 1024u;                 // 1 KiB/body
    const     std::size_t scaled       = static_cast<std::size_t>(max_bodies)
                                         * kBytesPerBody;
    return scaled > kFloor ? scaled : kFloor;
}

struct World {
    JPH::PhysicsSystem               system;
    JPH::TempAllocatorImpl           temp_allocator;
    JPH::JobSystemSingleThreaded     job_system{ JPH::cMaxPhysicsJobs };
    BPLayerInterfaceImpl             bp_layers;
    ObjectVsBPLayerFilterImpl        obj_vs_bp;
    ObjectLayerPairFilterImpl        obj_vs_obj;
    std::uint32_t                    tick_hz = 120;

    explicit World(std::uint32_t max_bodies)
        : temp_allocator(TempAllocSize(max_bodies)) {}
};

struct RigidBody {
    JPH::BodyID id;
    World*      owner = nullptr;
};

struct CharacterController {
    JPH::Ref<JPH::CharacterVirtual> character;
    World*                          owner          = nullptr;
    float                           capsule_radius = 0.40f;
    float                           capsule_height = 1.80f;
    float                           walk_speed     = 5.0f;
    float                           run_speed      = 8.0f;
    float                           jump_vel       = 4.5f;
    CharacterStance                 stance         = CharacterStance::Stand;
    float                           yaw_deg        = 0.0f;
    float                           vertical_velocity = 0.0f;
};

// ─── World lifecycle ─────────────────────────────────────────────────────

World* create_world(const WorldDesc& desc) {
    JoltGlobalInit();

    // Pass max_bodies through so the constructor sizes temp_allocator correctly.
    const std::uint32_t raw_max = desc.max_bodies > 0 ? desc.max_bodies : 65536u;
    auto* w = new (std::nothrow) World(raw_max);
    if (!w) {
        JoltGlobalShutdown();
        return nullptr;
    }

    // Cap body / pair / contact buffers at the configured maximum.
    // Jolt's own ranges are uint, so clamp to a sane upper bound.
    // raw_max was already clamped above; reuse it here to stay consistent.
    const JPH::uint max_bodies = static_cast<JPH::uint>(raw_max);
    const JPH::uint max_pairs  = max_bodies;
    const JPH::uint max_constr = desc.max_constraints > 0 ? desc.max_constraints
                                                          : max_bodies;

    w->system.Init(max_bodies,
                   /*num_body_mutexes=*/0,
                   max_pairs,
                   max_constr,
                   w->bp_layers,
                   w->obj_vs_bp,
                   w->obj_vs_obj);

    w->system.SetGravity(JPH::Vec3(0.0f, desc.gravity_y_m_per_s2, 0.0f));
    w->tick_hz = desc.tick_hz > 0 ? desc.tick_hz : 120u;
    return w;
}

void destroy_world(World* w) {
    if (!w) return;
    delete w;
    JoltGlobalShutdown();
}

void tick(World* w, float dt_seconds) {
    if (!w) return;
    // Single collision step per tick: at the design's 120 Hz fixed dt,
    // one step is stable for typical FPS scenes (DESIGN §10.1).
    constexpr int kCollisionSteps = 1;
    w->system.Update(dt_seconds, kCollisionSteps,
                     &w->temp_allocator, &w->job_system);
}

// ─── Rigid bodies ────────────────────────────────────────────────────────

RigidBody* create_body(World* w, const BodyDesc& d) {
    if (!w) return nullptr;
    JPH::ShapeRefC shape = MakeShape(d);
    if (shape == nullptr) return nullptr;

    const bool is_static = (d.mass_kg <= 0.0f);
    const JPH::EMotionType motion =
        is_static ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic;
    const JPH::ObjectLayer layer =
        is_static ? Layers::NON_MOVING : Layers::MOVING;

    JPH::BodyCreationSettings bcs(shape, R3(d.pos), Q(d.rot_quat), motion, layer);
    bcs.mFriction    = d.friction;
    bcs.mRestitution = d.restitution;
    if (!is_static) {
        bcs.mOverrideMassProperties =
            JPH::EOverrideMassProperties::CalculateInertia;
        bcs.mMassPropertiesOverride.mMass = d.mass_kg;
    }

    JPH::BodyInterface& bi = w->system.GetBodyInterface();
    const JPH::BodyID id = bi.CreateAndAddBody(
        bcs, is_static ? JPH::EActivation::DontActivate
                       : JPH::EActivation::Activate);
    if (id.IsInvalid()) return nullptr;

    auto* rb = new (std::nothrow) RigidBody{ id, w };
    if (!rb) {
        bi.RemoveBody(id);
        bi.DestroyBody(id);
    }
    return rb;
}

void destroy_body(World* w, RigidBody* rb) {
    if (!w || !rb) return;
    JPH::BodyInterface& bi = w->system.GetBodyInterface();
    if (!rb->id.IsInvalid()) {
        bi.RemoveBody(rb->id);
        bi.DestroyBody(rb->id);
    }
    delete rb;
}

void body_apply_force(RigidBody* rb, float fx, float fy, float fz) {
    if (!rb || !rb->owner || rb->id.IsInvalid()) return;
    JPH::BodyInterface& bi = rb->owner->system.GetBodyInterface();
    bi.AddForce(rb->id, JPH::Vec3(fx, fy, fz));
}

void body_get_transform(const RigidBody* rb, float out_pos[3], float out_quat[4]) {
    if (!rb || !rb->owner || rb->id.IsInvalid()) {
        out_pos[0] = out_pos[1] = out_pos[2] = 0.0f;
        out_quat[0] = out_quat[1] = out_quat[2] = 0.0f;
        out_quat[3] = 1.0f;
        return;
    }
    const JPH::BodyInterface& bi = rb->owner->system.GetBodyInterface();
    JPH::RVec3 p = bi.GetCenterOfMassPosition(rb->id);
    JPH::Quat  q = bi.GetRotation(rb->id);
    WriteV3(p, out_pos);
    WriteQ(q, out_quat);
}

// ─── Character controller ────────────────────────────────────────────────
//
// CharacterVirtual is Jolt's recommended FPS controller — it owns its
// own shape, slides along walls, handles step-up via WalkStairs, and is
// driven by setting linear velocity each tick. We expose a slim
// "input → tick → transform" loop that hides the Jolt setup.
//
// The capsule shape is rebuilt on stance changes (Stand/Crouch/Prone)
// so the controller's collision volume actually matches the visual
// posture. M4 will add lean / ladder / water modes; the public API
// already carries those bools, but they're no-ops until then.

namespace {

JPH::RefConst<JPH::Shape> MakeCharacterShape(float radius, float total_height) {
    // CharacterVirtual expects its shape to have its origin at the foot
    // (not centre-of-mass). The standard recipe is a capsule rotated /
    // translated so the origin lies on the bottom hemisphere. The shape
    // chosen is RotatedTranslatedShape(CapsuleShape(half_height, radius))
    // with the inner half-height = (total_height - 2*radius) / 2 and
    // translation = (0, half_height + radius, 0).
    const float half_segment =
        (total_height - 2.0f * radius) * 0.5f;
    const float clamped_half =
        half_segment > 0.0f ? half_segment : 0.01f;

    JPH::CapsuleShapeSettings cs(clamped_half, radius);
    cs.SetEmbedded();
    auto capsule_result = cs.Create();
    if (!capsule_result.IsValid()) return nullptr;

    JPH::RotatedTranslatedShapeSettings rts(
        JPH::Vec3(0.0f, clamped_half + radius, 0.0f),
        JPH::Quat::sIdentity(),
        capsule_result.Get());
    rts.SetEmbedded();
    auto rts_result = rts.Create();
    return rts_result.IsValid() ? rts_result.Get() : nullptr;
}

float StanceHeight(float base_height, psynder::physics::CharacterStance s) {
    using S = psynder::physics::CharacterStance;
    switch (s) {
        case S::Stand:  return base_height;
        case S::Crouch: return base_height * 0.5f;
        case S::Prone:  return base_height * 0.3f;
    }
    return base_height;
}

} // anonymous

CharacterController* create_character(World* w, const CharacterDesc& d) {
    if (!w) return nullptr;

    auto* cc = new (std::nothrow) CharacterController();
    if (!cc) return nullptr;
    cc->owner          = w;
    cc->capsule_radius = d.capsule_radius_m > 0.0f ? d.capsule_radius_m : 0.4f;
    cc->capsule_height = d.capsule_height_m > 0.0f ? d.capsule_height_m : 1.8f;
    cc->walk_speed     = d.walk_speed_mps;
    cc->run_speed      = d.run_speed_mps;
    cc->jump_vel       = d.jump_vel_mps;

    JPH::RefConst<JPH::Shape> shape =
        MakeCharacterShape(cc->capsule_radius, cc->capsule_height);
    if (shape == nullptr) {
        delete cc;
        return nullptr;
    }

    JPH::CharacterVirtualSettings cvs;
    cvs.mShape = shape;
    // SupportingVolume bounds the bottom of the capsule used for ground
    // detection. A plane at 0.05 m above the foot keeps the controller
    // standing on steps even with mild surface noise.
    cvs.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -cc->capsule_radius);
    cvs.mMaxSlopeAngle    = JPH::DegreesToRadians(50.0f);
    cvs.mMass             = 80.0f;       // kg (typical adult)
    cvs.mMaxStrength      = 200.0f;      // N — push force ceiling
    cvs.mShapeOffset      = JPH::Vec3::sZero();

    cc->character = new JPH::CharacterVirtual(
        &cvs, R3(d.pos), JPH::Quat::sIdentity(), 0ULL, &w->system);
    return cc;
}

void destroy_character(World* w, CharacterController* cc) {
    (void)w;
    if (!cc) return;
    cc->character = nullptr; // release Jolt ref
    delete cc;
}

void character_tick(CharacterController* cc,
                    const CharacterInput& in,
                    float dt) {
    if (!cc || !cc->owner || cc->character == nullptr || dt <= 0.0f) return;

    // ── 1. Rebuild capsule on stance change ──
    CharacterStance want = CharacterStance::Stand;
    if (in.prone)  want = CharacterStance::Prone;
    else if (in.crouch) want = CharacterStance::Crouch;
    if (want != cc->stance) {
        const float new_height =
            StanceHeight(cc->capsule_height, want);
        JPH::RefConst<JPH::Shape> new_shape =
            MakeCharacterShape(cc->capsule_radius, new_height);
        if (new_shape != nullptr) {
            // Best-effort shape swap; fail silently if the new capsule
            // can't fit (e.g. trying to stand under a low ceiling) and
            // leave the previous stance in place.
            const bool ok = cc->character->SetShape(
                new_shape,
                /*max_penetration_depth=*/0.05f,
                cc->owner->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                cc->owner->system.GetDefaultLayerFilter(Layers::MOVING),
                /*body_filter=*/{},
                /*shape_filter=*/{},
                cc->owner->temp_allocator);
            if (ok) cc->stance = want;
        }
    }

    // ── 2. Build planar move vector in character-local axes ──
    // For Wave-B we keep input in world space (no yaw rotation yet);
    // the FPS camera will own yaw and feed pre-rotated XY values once
    // lane 06 (scene) wires the camera component. This keeps the
    // character self-contained.
    const float mx = in.move_xy[0];
    const float mz = in.move_xy[1];
    float planar_len = std::sqrt(mx * mx + mz * mz);
    if (planar_len > 1.0f) planar_len = 1.0f;
    const float speed = cc->run_speed; // sprint by default; lane 06 hooks the toggle later

    JPH::Vec3 desired_velocity(
        mx * speed,
        cc->character->GetLinearVelocity().GetY(),
        mz * speed);

    // ── 3. Gravity + jump ──
    const JPH::Vec3 gravity = cc->owner->system.GetGravity();
    if (cc->character->GetGroundState() ==
        JPH::CharacterVirtual::EGroundState::OnGround) {
        cc->vertical_velocity = 0.0f;
        if (in.jump) {
            cc->vertical_velocity = cc->jump_vel;
        }
    } else {
        cc->vertical_velocity += gravity.GetY() * dt;
    }
    desired_velocity.SetY(cc->vertical_velocity);
    cc->character->SetLinearVelocity(desired_velocity);

    // ── 4. Lean is cosmetic (camera roll); handled by render lane. ──
    (void)in.lean_left;
    (void)in.lean_right;

    // ── 5. Integrate. Use the basic Update (no stair-walk / stick) for
    //      the bootstrap; ExtendedUpdate wires in once the scene exposes
    //      stair-step preferences in M4. ──
    cc->character->Update(
        dt,
        gravity,
        cc->owner->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
        cc->owner->system.GetDefaultLayerFilter(Layers::MOVING),
        /*body_filter=*/{},
        /*shape_filter=*/{},
        cc->owner->temp_allocator);
}

void character_get_transform(const CharacterController* cc,
                             float out_pos[3],
                             float out_yaw_deg) {
    // out_yaw_deg is a scalar (not a pointer) in the frozen contract;
    // the parameter is accepted but cannot be written through. We treat
    // it as informational input from the camera lane in a later wave.
    (void)out_yaw_deg;
    if (!cc || cc->character == nullptr) {
        out_pos[0] = out_pos[1] = out_pos[2] = 0.0f;
        return;
    }
    WriteV3(cc->character->GetPosition(), out_pos);
}

} // namespace psynder::physics
