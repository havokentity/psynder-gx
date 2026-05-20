// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/vehicle/Vehicle.cpp
//
// Lane 16 — real vehicle dynamics: a 3-DOF-translation + full-orientation
// rigid-body chassis riding on raycast suspension, with a Pacejka-lite
// combined-slip tire model and a drivetrain that runs engine torque curve ->
// clutch -> gearbox -> differential -> wheels, plus aerodynamic drag and
// downforce. Real SI units throughout (DESIGN §10.1).
//
// ─── Why the sim is self-contained ──────────────────────────────────────────
// The frozen physics-core contract (PublicPhysicsCore.h) exposes only
// create_body / body_apply_force (linear, at the COM) / body_get_transform —
// there is no ray cast, no point/torque application, and no velocity readback.
// A faithful raycast-suspension vehicle needs all of those, so the chassis is
// integrated here instead of being driven through a Jolt body. The World* is
// retained for the future broadphase hookup; the ground is currently an
// analytic plane (see raycast_ground). See the PR body for the follow-up that
// would let this defer to physics-core once those queries are exposed.
//
// ─── Real-metric calibration (from the original lane-16 stub, kept verbatim) ──
//  MASSES (kg)   jeep 1800 | sedan 1200 | sports 1450 | truck 7000 | tank 12000
//  PEAK TORQUE   jeep 250@2600 | sedan 200@2000 | sports 400@5500 | diesel 850
//  TIRE mu       dry 1.05 | wet 0.65 | gravel 0.55 | snow/ice 0.20 | mud 0.45
//  AERO          sedan Cd .30 A 2.2 | SUV .38/2.8 | sports .28/1.9 | tank .65/6.8
//                rho_air 1.204 kg/m^3,  drag = 0.5*rho*v^2*Cd*A
//  SUSPENSION    rest 0.35 | k 22000 N/m | c 2800 N*s/m | gravity -9.81 m/s^2
//
// DETERMINISM: -fno-fast-math (see CMakeLists.txt) plus the fixed operation
// order here give bit-identical replay for a given platform/toolchain (the
// determinism unit test pins this). It is NOT sufficient for cross-platform
// lockstep on its own, because the tick calls libm transcendentals
// (sin/cos/atan/atan2/sqrt/ceil) that differ bitwise across libm versions;
// cross-host bit-identity needs a deterministic transcendental layer, a
// follow-up shared with physics-core (cf. Jolt CROSS_PLATFORM_DETERMINISTIC).

#include "physics/core/PublicPhysicsCore.h"
#include "physics/vehicle/PublicVehicle.h"
#include "physics/vehicle/VehicleModel.h"

#include <cmath>
#include <cstdint>

namespace psynder::physics::vehicle {
namespace {

// ─── Capacity caps (fixed budgets keep the node allocation-free in tick) ──────
constexpr std::uint32_t kMaxWheels = 16;
constexpr std::uint32_t kMaxGears = 16;
constexpr std::uint32_t kMaxCurve = 32;

// ─── Tuning policy (documented real-ish defaults) ─────────────────────────────
constexpr float kMaxSteerRad = 0.6109f;        // 35 deg at full lock
constexpr float kBrakeTorqueNm = 3000.0f;      // per braked wheel at full pedal
constexpr float kHandbrakeTorqueNm = 4000.0f;  // added to rear braked wheels
constexpr float kEngineInertiaKgM2 = 0.30f;    // flywheel + rotating assembly
constexpr float kWheelMassKg = 25.0f;          // for solid-disc wheel inertia
constexpr float kIdleRpm = 900.0f;
constexpr float kSlipEps = 1.0f;               // m/s denominator floor for slip
constexpr float kEngineBrakeNm = 25.0f;        // closed-throttle drag (scaled by rpm)
constexpr float kDownforceClaPerArea = 0.15f;  // derived lift area (no desc field yet)
constexpr float kSubStepDt = 1.0f / 240.0f;    // internal integration step
constexpr int kMaxSubSteps = 64;
constexpr float kGroundHeight = 0.0f;          // analytic flat ground plane (y)
constexpr float kJounceFraction = 0.4f;        // usable bump travel as frac of rest
constexpr float kReboundFraction = 0.5f;       // extra droop ray length as frac of rest
constexpr float kMinDim = 0.5f;                // floor for derived chassis extents

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float wheel_inertia(const WheelDesc& wd) {
    // Solid-disc approximation I = 1/2 m r^2.
    return 0.5f * kWheelMassKg * wd.tire_radius_m * wd.tire_radius_m;
}

// Brake torque magnitude for a wheel given the driver input. The handbrake
// adds to rear braked wheels (local +Z is forward, so rear is z < 0), which is
// what lets a handbrake yank the tail out under the friction-circle limiter.
inline float brake_torque(const VehicleInput& in, const WheelDesc& wd) {
    if (!wd.is_braked) return 0.0f;
    float t = clampf(in.brake, 0.0f, 1.0f) * kBrakeTorqueNm;
    if (in.handbrake && wd.local_pos[2] < 0.0f) t += kHandbrakeTorqueNm;
    return t;
}

// Decelerate a wheel's spin toward zero under a brake torque without
// overshooting into reverse (a locked wheel sits at omega = 0).
inline float brake_spin(float omega, float brake_t, float inertia, float dt) {
    if (brake_t <= 0.0f || inertia <= 0.0f) return omega;
    const float dw = (brake_t / inertia) * dt;
    if (omega > 0.0f) return omega > dw ? omega - dw : 0.0f;
    if (omega < 0.0f) return omega < -dw ? omega + dw : 0.0f;
    return 0.0f;
}

inline int gear_rank(float ratio) { return ratio < 0.0f ? 0 : (ratio > 0.0f ? 2 : 1); }

// Reorder gears into a canonical sequential layout so a relative gear_request
// works regardless of how the caller laid out VehicleDesc::gear_ratios — the
// public header's example ({R, N, 1st..}) and its prose ("forward gears first,
// then reverse") disagree on reverse placement. Canonical sequence: reverse
// gears (ascending), neutral(s), then forward gears descending by ratio (so the
// first forward gear has the highest ratio). Stable insertion sort, n <= 16.
inline void canonicalize_gears(const float* src, std::uint32_t n, float* dst) {
    for (std::uint32_t i = 0; i < n; ++i) dst[i] = src[i];
    for (std::uint32_t i = 1; i < n; ++i) {
        const float key = dst[i];
        std::uint32_t j = i;
        while (j > 0) {
            const float prev = dst[j - 1];
            const int rp = gear_rank(prev);
            const int rk = gear_rank(key);
            bool move;
            if (rp != rk) {
                move = rp > rk;       // lower rank first (reverse < neutral < forward)
            } else if (rk == 2) {
                move = prev < key;    // forward gears: descending ratio
            } else {
                move = prev > key;    // reverse: ascending; neutral: stable (no move)
            }
            if (!move) break;
            dst[j] = prev;
            --j;
        }
        dst[j] = key;
    }
}

// Analytic ground query: ray from `origin` along `axis_down` (unit, pointing
// roughly toward -Y) against a flat plane at y = kGroundHeight. Returns the hit
// distance along the axis. This is the single seam that a future physics-core
// broadphase ray cast would replace.
inline bool raycast_ground(Vec3 origin, Vec3 axis_down, float max_len, float& out_dist) {
    const float denom = axis_down.y;
    if (denom >= -1e-4f) return false;  // axis not pointing roughly downward
    const float t = (kGroundHeight - origin.y) / denom;
    if (t > max_len) return false;
    // t < 0 means the attach point is at/below the plane (penetration); treat it
    // as an immediate hit at distance 0 so the suspension drives the chassis
    // back out instead of mistaking it for an airborne wheel.
    out_dist = t > 0.0f ? t : 0.0f;
    return true;
}

}  // namespace

// ─── Vehicle state ────────────────────────────────────────────────────────────

struct WheelRuntime {
    WheelDesc desc{};
    float spin_omega = 0.0f;   // rad/s
    float compression = 0.0f;  // m, > 0 = compressed past rest
    bool grounded = false;
};

struct Vehicle {
    // Cached configuration (caller arrays are borrowed, so copy on create).
    float mass_kg = 1200.0f;
    float inv_mass = 1.0f / 1200.0f;
    Vec3 inertia{};
    Vec3 inv_inertia{};
    float drag_cd = 0.30f;
    float frontal_area_m2 = 2.0f;
    float lift_area_m2 = 0.0f;
    float com_offset_y = 0.0f;
    Drivetrain drivetrain = Drivetrain::Rwd;
    float final_drive = 3.5f;
    float redline_rpm = 7000.0f;

    std::uint32_t wheel_count = 0;
    WheelRuntime wheels[kMaxWheels];

    std::uint32_t gear_count = 0;
    std::uint32_t gear_index = 0;
    float gear_ratios[kMaxGears] = {};

    std::uint32_t curve_count = 0;
    EngineCurvePoint curve[kMaxCurve] = {};

    // Rigid-body state, COM frame.
    Vec3 position{};
    Quat orientation{};
    Vec3 lin_vel{};
    Vec3 ang_vel{};  // world frame, rad/s
    float engine_rpm = kIdleRpm;

    ::psynder::physics::World* world = nullptr;  // reserved for broadphase hookup
};

namespace {

// One semi-implicit Euler sub-step of the full vehicle dynamics.
void substep(Vehicle& v, const VehicleInput& in, float steer_rad, float dt) {
    const Vec3 fwd = quat_rotate(v.orientation, {0.0f, 0.0f, 1.0f});
    const Vec3 right = quat_rotate(v.orientation, {1.0f, 0.0f, 0.0f});
    const Vec3 up = quat_rotate(v.orientation, {0.0f, 1.0f, 0.0f});
    const Vec3 axis_down = -up;

    // ── Drivetrain: derive engine speed/torque from the driven-wheel speeds ──
    float driven_omega_sum = 0.0f;
    int driven = 0;
    for (std::uint32_t i = 0; i < v.wheel_count; ++i) {
        if (v.wheels[i].desc.is_driven) {
            driven_omega_sum += v.wheels[i].spin_omega;
            ++driven;
        }
    }
    const float avg_driven_omega = driven > 0 ? driven_omega_sum / static_cast<float>(driven)
                                              : 0.0f;
    const float ratio = v.gear_ratios[v.gear_index];  // signed; 0 = neutral
    const float trans = ratio * v.final_drive;
    const float throttle = clampf(in.throttle, 0.0f, 1.0f);

    float wheel_drive_torque = 0.0f;
    float reflected_inertia = 0.0f;
    if (ratio != 0.0f && driven > 0) {
        // Clutch locked: engine speed reflects the driven-wheel speed, floored
        // at idle so the engine never stalls (clutch slip at launch).
        const float raw_rpm = rad_per_sec_to_rpm(avg_driven_omega * trans);
        const float rpm = clampf(raw_rpm, kIdleRpm, v.redline_rpm);
        v.engine_rpm = rpm;
        float t_engine = engine_torque_at(v.curve, v.curve_count, rpm) * throttle;
        if (throttle < 0.01f) t_engine -= kEngineBrakeNm * (rpm / v.redline_rpm);
        // Rev limiter (fuel cut at redline) — this is what caps a gear's top
        // speed and makes upshifting the only way to keep accelerating.
        if (raw_rpm >= v.redline_rpm) t_engine = 0.0f;
        wheel_drive_torque = t_engine * trans / static_cast<float>(driven);
        reflected_inertia = kEngineInertiaKgM2 * trans * trans;
    } else {
        // Neutral (or no driven wheels): free-rev the engine on its own inertia.
        float t_engine = engine_torque_at(v.curve, v.curve_count, v.engine_rpm) * throttle;
        if (throttle < 0.01f) t_engine -= kEngineBrakeNm * (v.engine_rpm / v.redline_rpm);
        float omega = v.engine_rpm * kRpmToRadPerSec + (t_engine / kEngineInertiaKgM2) * dt;
        v.engine_rpm = clampf(rad_per_sec_to_rpm(omega), kIdleRpm, v.redline_rpm);
    }

    // ── Accumulate forces/torques about the COM ──
    Vec3 force{0.0f, -v.mass_kg * kGravity, 0.0f};  // gravity
    Vec3 torque{0.0f, 0.0f, 0.0f};

    const float speed = length(v.lin_vel);
    if (speed > 1e-4f) {
        const Vec3 vdir = v.lin_vel * (1.0f / speed);
        force = force - vdir * aero_drag_force(v.drag_cd, v.frontal_area_m2, speed);
    }
    force.y -= aero_downforce(v.lift_area_m2, speed);  // downforce presses world-down

    for (std::uint32_t i = 0; i < v.wheel_count; ++i) {
        WheelRuntime& wheel = v.wheels[i];
        const WheelDesc& wd = wheel.desc;
        const bool was_grounded = wheel.grounded;
        const float inertia = wheel_inertia(wd) + (wd.is_driven ? reflected_inertia : 0.0f);
        const float bt = brake_torque(in, wd);

        const Vec3 local_attach{wd.local_pos[0], wd.local_pos[1] - v.com_offset_y,
                                wd.local_pos[2]};
        const Vec3 attach_world = v.position + quat_rotate(v.orientation, local_attach);
        const float max_len = wd.suspension_rest_m + wd.tire_radius_m +
                              wd.suspension_rest_m * kReboundFraction;

        float hit_dist = 0.0f;
        if (!raycast_ground(attach_world, axis_down, max_len, hit_dist)) {
            // Airborne: no road reaction; the wheel only responds to drive/brake.
            wheel.grounded = false;
            wheel.compression = 0.0f;
            float omega = wheel.spin_omega +
                          ((wd.is_driven ? wheel_drive_torque : 0.0f) / inertia) * dt;
            wheel.spin_omega = brake_spin(omega, bt, inertia, dt);
            continue;
        }
        wheel.grounded = true;

        const float center_dist = hit_dist - wd.tire_radius_m;  // attach -> wheel center
        const float compression = wd.suspension_rest_m - center_dist;
        // Zero the rate on the first grounded frame so the contact transition
        // doesn't inject a spurious damping impulse that launches the chassis.
        const float comp_rate = was_grounded ? (compression - wheel.compression) / dt : 0.0f;
        wheel.compression = compression;
        const float jounce = wd.suspension_rest_m * kJounceFraction;
        const float load = suspension_force(wd.suspension_stiffness, wd.suspension_damping,
                                            compression, comp_rate, jounce);

        if (load <= 0.0f) {
            // In droop / no load: support nothing, no tire grip; spin still evolves.
            float omega = wheel.spin_omega +
                          ((wd.is_driven ? wheel_drive_torque : 0.0f) / inertia) * dt;
            wheel.spin_omega = brake_spin(omega, bt, inertia, dt);
            continue;
        }

        // Suspension pushes the chassis up along its axis.
        const Vec3 contact = attach_world + axis_down * (center_dist + wd.tire_radius_m);
        const Vec3 r_contact = contact - v.position;
        force = force + up * load;
        torque = torque + cross(r_contact, up * load);

        // Tire ground frame: chassis fwd/right rotated by steer about up, then
        // flattened into the ground plane so traction stays horizontal.
        const float steer = wd.is_steered ? steer_rad : 0.0f;
        const float cs = std::cos(steer);
        const float sn = std::sin(steer);
        const Vec3 wheel_fwd = fwd * cs + right * sn;
        const Vec3 wheel_lat = right * cs - fwd * sn;
        const Vec3 ground_fwd = normalize(Vec3{wheel_fwd.x, 0.0f, wheel_fwd.z});
        const Vec3 ground_lat = normalize(Vec3{wheel_lat.x, 0.0f, wheel_lat.z});

        const Vec3 v_contact = v.lin_vel + cross(v.ang_vel, r_contact);
        const float v_long = dot(v_contact, ground_fwd);
        const float v_lat = dot(v_contact, ground_lat);

        const float surface_speed = wheel.spin_omega * wd.tire_radius_m;
        const float kappa = slip_ratio(surface_speed, v_long, kSlipEps);
        const float alpha = slip_angle(v_long, v_lat, kSlipEps);
        float f_long = tire_mu_long(kappa, wd.tire_friction_mu) * load;
        float f_lat = -tire_mu_lat(alpha, wd.tire_friction_mu) * load;  // opposes slip
        clamp_friction_circle(f_long, f_lat, wd.tire_friction_mu * load);

        const Vec3 tire_force = ground_fwd * f_long + ground_lat * f_lat;
        force = force + tire_force;
        torque = torque + cross(r_contact, tire_force);

        // Wheel spin: drive torque minus road reaction (traction * radius),
        // then brake toward zero.
        float net = (wd.is_driven ? wheel_drive_torque : 0.0f) - f_long * wd.tire_radius_m;
        float omega = wheel.spin_omega + (net / inertia) * dt;
        wheel.spin_omega = brake_spin(omega, bt, inertia, dt);
    }

    // ── Integrate the rigid body (semi-implicit Euler) ──
    v.lin_vel = v.lin_vel + force * (v.inv_mass * dt);
    v.position = v.position + v.lin_vel * dt;

    // Angular: torque (world) -> body frame -> diagonal inverse inertia -> world.
    const Quat conj{-v.orientation.x, -v.orientation.y, -v.orientation.z, v.orientation.w};
    const Vec3 torque_body = quat_rotate(conj, torque);
    const Vec3 ang_acc_body{torque_body.x * v.inv_inertia.x, torque_body.y * v.inv_inertia.y,
                            torque_body.z * v.inv_inertia.z};
    v.ang_vel = v.ang_vel + quat_rotate(v.orientation, ang_acc_body) * dt;
    v.orientation = quat_integrate(v.orientation, v.ang_vel, dt);
}

}  // namespace

// ─── Public API ───────────────────────────────────────────────────────────────

Vehicle* create_vehicle(::psynder::physics::World* world, const VehicleDesc& desc) {
    Vehicle* v = new Vehicle;
    v->world = world;
    v->mass_kg = desc.mass_kg > 1.0f ? desc.mass_kg : 1.0f;
    v->inv_mass = 1.0f / v->mass_kg;
    v->drag_cd = desc.drag_cd;
    v->frontal_area_m2 = desc.frontal_area_m2;
    v->lift_area_m2 = desc.frontal_area_m2 * kDownforceClaPerArea;
    // NOTE: despite the "_z_m" name, this field is documented in PublicVehicle.h
    // as the offset "below geometric center" — i.e. a vertical (Y, up-axis)
    // offset in this Y-up engine, not a Z offset. We apply it on Y accordingly.
    // The header is frozen, so the misleading name stays; this note guards
    // against callers populating it as a longitudinal (Z) offset.
    v->com_offset_y = desc.center_of_mass_offset_z_m;
    v->drivetrain = desc.drivetrain;
    v->final_drive = desc.final_drive;

    // Copy the wheel array (clamped to capacity; tolerate a null pointer).
    v->wheel_count = desc.wheels == nullptr
                         ? 0u
                         : (desc.wheel_count < kMaxWheels ? desc.wheel_count : kMaxWheels);
    float min_x = 1e9f, max_x = -1e9f, min_z = 1e9f, max_z = -1e9f, sum_w = 0.0f;
    for (std::uint32_t i = 0; i < v->wheel_count; ++i) {
        const WheelDesc& wd = desc.wheels[i];
        v->wheels[i].desc = wd;
        min_x = wd.local_pos[0] < min_x ? wd.local_pos[0] : min_x;
        max_x = wd.local_pos[0] > max_x ? wd.local_pos[0] : max_x;
        min_z = wd.local_pos[2] < min_z ? wd.local_pos[2] : min_z;
        max_z = wd.local_pos[2] > max_z ? wd.local_pos[2] : max_z;
        sum_w += wd.tire_width_m;
    }

    // Copy the gearbox (clamped) and start in neutral if the array has one.
    v->gear_count = desc.gear_ratios == nullptr
                        ? 0u
                        : (desc.gear_count < kMaxGears ? desc.gear_count : kMaxGears);
    // Reorder into a canonical R / N / forward sequence so sequential shifting
    // is robust to the caller's array layout, then start in neutral.
    canonicalize_gears(desc.gear_ratios, v->gear_count, v->gear_ratios);
    v->gear_index = 0;
    for (std::uint32_t i = 0; i < v->gear_count; ++i) {
        if (v->gear_ratios[i] == 0.0f) {
            v->gear_index = i;
            break;
        }
    }

    // Copy the engine curve (clamped); redline is the top sampled rpm.
    v->curve_count = desc.engine_curve == nullptr
                         ? 0u
                         : (desc.engine_curve_n < kMaxCurve ? desc.engine_curve_n : kMaxCurve);
    for (std::uint32_t i = 0; i < v->curve_count; ++i) v->curve[i] = desc.engine_curve[i];
    v->redline_rpm = v->curve_count > 0 ? v->curve[v->curve_count - 1].rpm : 7000.0f;
    if (v->redline_rpm < kIdleRpm + 1.0f) v->redline_rpm = kIdleRpm + 1.0f;
    v->engine_rpm = kIdleRpm;

    // Derive chassis extents (track / wheelbase / height-from-frontal-area) and
    // the principal box inertia about the COM.
    const float avg_w = v->wheel_count > 0 ? sum_w / static_cast<float>(v->wheel_count) : 0.2f;
    float width = (max_x - min_x) + avg_w;
    float length = (max_z - min_z) * 1.15f;
    width = width > kMinDim ? width : 1.6f;
    length = length > kMinDim ? length : 3.0f;
    float height = clampf(v->frontal_area_m2 / width, 0.8f, 3.0f);
    const float m = v->mass_kg;
    v->inertia = {(1.0f / 12.0f) * m * (height * height + length * length),
                  (1.0f / 12.0f) * m * (width * width + length * length),
                  (1.0f / 12.0f) * m * (width * width + height * height)};
    v->inv_inertia = {1.0f / v->inertia.x, 1.0f / v->inertia.y, 1.0f / v->inertia.z};

    // Spawn pose: identity orientation, COM at a height that lets the
    // suspension settle from a small drop rather than clipping the ground.
    float rest_max = 0.35f;
    for (std::uint32_t i = 0; i < v->wheel_count; ++i) {
        const float r = v->wheels[i].desc.suspension_rest_m + v->wheels[i].desc.tire_radius_m;
        rest_max = r > rest_max ? r : rest_max;
    }
    v->position = {0.0f, kGroundHeight + rest_max + 0.05f, 0.0f};
    v->orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    return v;
}

void destroy_vehicle(Vehicle* v) { delete v; }

void vehicle_tick(Vehicle* v, const VehicleInput& input, float dt) {
    if (v == nullptr || dt <= 0.0f) return;

    // Discrete sequential shift, once per tick.
    if (input.gear_request > 0 && v->gear_index + 1 < v->gear_count) {
        ++v->gear_index;
    } else if (input.gear_request < 0 && v->gear_index > 0) {
        --v->gear_index;
    }

    const float steer_rad = clampf(input.steer, -1.0f, 1.0f) * kMaxSteerRad;

    // Substep to a fixed internal dt so behaviour is robust to the caller's dt
    // and the integration stays stable with stiff springs / reflected inertia.
    int steps = static_cast<int>(std::ceil(dt / kSubStepDt));
    if (steps < 1) steps = 1;
    if (steps > kMaxSubSteps) steps = kMaxSubSteps;
    const float sub_dt = dt / static_cast<float>(steps);
    for (int s = 0; s < steps; ++s) substep(*v, input, steer_rad, sub_dt);
}

void vehicle_get_transform(const Vehicle* v, float out_pos[3], float out_quat[4]) {
    if (v == nullptr) {
        if (out_pos != nullptr) out_pos[0] = out_pos[1] = out_pos[2] = 0.0f;
        if (out_quat != nullptr) {
            out_quat[0] = out_quat[1] = out_quat[2] = 0.0f;
            out_quat[3] = 1.0f;
        }
        return;
    }
    if (out_pos != nullptr) {
        out_pos[0] = v->position.x;
        out_pos[1] = v->position.y;
        out_pos[2] = v->position.z;
    }
    if (out_quat != nullptr) {
        out_quat[0] = v->orientation.x;
        out_quat[1] = v->orientation.y;
        out_quat[2] = v->orientation.z;
        out_quat[3] = v->orientation.w;
    }
}

float vehicle_get_speed_mps(const Vehicle* v) { return v != nullptr ? length(v->lin_vel) : 0.0f; }

}  // namespace psynder::physics::vehicle
