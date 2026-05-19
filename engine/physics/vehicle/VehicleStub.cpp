// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/vehicle/VehicleStub.cpp
//
// Lane 16 — Vehicle stub implementation (Wave B / M7 scaffold).
//
// ─── M7 REAL-METRIC TARGET VALUES (NO SHORTCUTS) ─────────────────────────
//
// When the full M7 implementation lands, the following physics constants
// MUST be used verbatim. All values are SI / real-world-calibrated.
//
//  MASSES (kg)
//    Light jeep / off-road      :  1 800 kg   (e.g. Land Rover Defender 110)
//    Compact sedan              :  1 200 kg   (e.g. VW Golf)
//    Sports car                 :  1 450 kg   (e.g. Porsche 911 GT3)
//    Light military truck       :  7 000 kg   (e.g. HMMWV)
//    Main battle tank (tracked) : 12 000 kg   (design §10.1 "light tank")
//
//  PEAK TORQUE (N·m at crank) — engine curve stored in EngineCurvePoint[]
//    Jeep/off-road   : 250 N·m  @ ~2 600 rpm
//    Sedan           : 200 N·m  @ ~2 000 rpm
//    Sports car      : 400 N·m  @ ~5 500 rpm
//    Diesel truck    : 850 N·m  @ ~1 500 rpm
//    Tank turbine    : 1 800 N·m (flat, turbine curve)
//
//  TIRE FRICTION µ (Pacejka-lite peak lateral/longitudinal)
//    Dry tarmac      : µ = 1.05  (nominal; range 1.0–1.2 per §10.1)
//    Wet tarmac      : µ = 0.65
//    Gravel / dirt   : µ = 0.55
//    Snow / ice      : µ = 0.20
//    Off-road mud    : µ = 0.45
//
//  AERODYNAMIC DRAG (CdA = Cd × frontal_area_m²)
//    Sedan    : Cd = 0.30,  A = 2.20 m²  → CdA = 0.66
//    SUV/jeep : Cd = 0.38,  A = 2.80 m²  → CdA = 1.06
//    Sports   : Cd = 0.28,  A = 1.90 m²  → CdA = 0.53
//    Tank     : Cd = 0.65,  A = 6.80 m²  → CdA = 4.42
//    ρ_air = 1.204 kg/m³ at 20 °C / sea level
//    Drag force = 0.5 × ρ × v² × CdA
//
//  PACEJKA MAGIC FORMULA (lite — B, C, D, E coefficients)
//    Lateral (cornering):
//      B = 10.0, C = 1.9, D = µ_peak, E = 0.97
//      F_y = D × sin(C × atan(B×α − E×(B×α − atan(B×α))))
//      where α = slip angle (rad)
//    Longitudinal (traction/braking):
//      B = 11.0, C = 1.65, D = µ_peak, E = 0.89
//      F_x = D × sin(C × atan(B×κ − E×(B×κ − atan(B×κ))))
//      where κ = longitudinal slip ratio
//    Combined slip: Pacejka combined weighting (cos²-blend, see §10.1)
//
//  SUSPENSION (defaults for sedan — override per VehicleDesc wheel array)
//    rest_length      : 0.35 m
//    stiffness        : 22 000 N/m
//    damping          : 2 800 N·s/m
//    travel (full jounce) : 0.12 m above rest
//    travel (full rebound): 0.20 m below rest
//    anti-roll bar    : 800 N·m/rad
//
//  GRAVITY (taken from psynder::physics::World / WorldDesc.gravity_y_m_per_s2)
//    g = −9.81 m/s²  (1 world unit = 1 metre, per §3 / DESIGN §10.1)
//
//  -fno-fast-math is mandatory for all vehicle TUs (see lane CMakeLists.txt).
//  No demo-scaled shortcuts, no fudge factors.  Real constants or TODO.
// ─────────────────────────────────────────────────────────────────────────

// PublicPhysicsCore.h must precede PublicVehicle.h: the vehicle header
// references ::psynder::physics::World which is forward-declared there.
#include "physics/core/PublicPhysicsCore.h"
#include "physics/vehicle/PublicVehicle.h"

#include <cstring>   // memcpy

namespace psynder::physics::vehicle {

// ── Internal node ────────────────────────────────────────────────────────────
// VehicleStub keeps a singly-linked list so create/destroy need no allocator.

struct Vehicle {
    float           pos[3]  = {0.f, 0.f, 0.f};
    float           quat[4] = {0.f, 0.f, 0.f, 1.f}; // identity {x,y,z,w}
    ::psynder::physics::World* world = nullptr;
    Vehicle* next = nullptr;
};

namespace {
    Vehicle* g_head = nullptr;
} // anonymous namespace

// ── create_vehicle ────────────────────────────────────────────────────────────
//
// Allocates a Vehicle node, captures the spawn pose from VehicleDesc (using
// the center-of-mass offset as the initial Y offset so the vehicle isn't
// clipping into ground at construction), links it at list head.
// Full Pacejka/drivetrain/suspension wiring is deferred to M7 implementation.
//
Vehicle* create_vehicle(::psynder::physics::World* world, const VehicleDesc& desc)
{
    // Stub: plain new is acceptable here; M7 will use the psynder job-system
    // slab allocator (psynder_jobs / JobAlloc).
    Vehicle* v = new Vehicle;
    v->world  = world;
    // Spawn at origin with identity quaternion; M7 will accept a spawn pose.
    v->pos[0] = 0.f;
    v->pos[1] = desc.center_of_mass_offset_z_m; // intentional: keep API coupling
    v->pos[2] = 0.f;
    v->quat[0] = 0.f; v->quat[1] = 0.f; v->quat[2] = 0.f; v->quat[3] = 1.f;
    // Link at head
    v->next  = g_head;
    g_head   = v;
    return v;
}

// ── destroy_vehicle ───────────────────────────────────────────────────────────
//
// Unlinks the node from the global list and deletes it.
// No-op if v is null.
//
void destroy_vehicle(Vehicle* v)
{
    if (!v) return;
    // Unlink from singly-linked list
    if (g_head == v) {
        g_head = v->next;
    } else {
        for (Vehicle* cur = g_head; cur && cur->next; cur = cur->next) {
            if (cur->next == v) {
                cur->next = v->next;
                break;
            }
        }
    }
    delete v;
}

// ── vehicle_tick ──────────────────────────────────────────────────────────────
//
// No-op stub.  M7 will:
//   1. Run Pacejka slip-angle / slip-ratio per wheel (see metric values above).
//   2. Apply raycast suspension forces against the physics World.
//   3. Integrate drivetrain torque (engine curve × gear ratio × final_drive).
//   4. Feed aerodynamic drag force = 0.5 × 1.204 × v² × CdA.
//   5. Resolve combined-slip friction and apply wheel impulses.
//   6. Advance rigid-body pose via psynder::physics::tick().
//
void vehicle_tick(Vehicle* /*v*/, const VehicleInput& /*input*/, float /*dt*/)
{
    // Stub: intentional no-op.
}

// ── vehicle_get_transform ─────────────────────────────────────────────────────
//
// Returns the spawn pose captured at create_vehicle time.
// M7 will copy the live rigid-body transform from the physics World.
//
void vehicle_get_transform(const Vehicle* v, float out_pos[3], float out_quat[4])
{
    if (!v) {
        if (out_pos)  { out_pos[0] = out_pos[1] = out_pos[2] = 0.f; }
        if (out_quat) { out_quat[0] = out_quat[1] = out_quat[2] = 0.f; out_quat[3] = 1.f; }
        return;
    }
    if (out_pos)  std::memcpy(out_pos,  v->pos,  3 * sizeof(float));
    if (out_quat) std::memcpy(out_quat, v->quat, 4 * sizeof(float));
}

// ── vehicle_get_speed_mps ─────────────────────────────────────────────────────
//
// Returns 0.0 m/s; no integration is performed in the stub.
// M7 will derive speed from the rigid-body's linear velocity magnitude.
//
float vehicle_get_speed_mps(const Vehicle* /*v*/)
{
    return 0.0f; // m/s — stub
}

} // namespace psynder::physics::vehicle
