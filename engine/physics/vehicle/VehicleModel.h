// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/vehicle/VehicleModel.h
//
// Lane 16 — pure vehicle-dynamics math (header-inline, no state).
//
// Everything here is a free function or a constant so it can be unit-tested
// in isolation and inlined into the -fno-fast-math TU (Vehicle.cpp). All
// values are SI / real-world calibrated; see PublicVehicle.h and DESIGN
// §10.1. Coefficients are pinned to the Pacejka-lite figures recorded in the
// original lane-16 stub so the contract is preserved verbatim.
//
//   Lateral  : B = 10.0, C = 1.9,  E = 0.97
//   Longitud.: B = 11.0, C = 1.65, E = 0.89
//   F = D * sin(C * atan(B*s - E*(B*s - atan(B*s))))   (D = mu_peak)
//   rho_air  = 1.204 kg/m^3,  drag = 0.5 * rho * Cd * A * v^2
//
// The vector / quaternion helpers are kept local (rather than pulling in
// psynder::math, whose quaternion ops are out-of-line in a TU that is not
// compiled -fno-fast-math) so the integrator's arithmetic stays inside this
// lane's -fno-fast-math TU and replays bit-identically for a given
// platform/toolchain (what the determinism test pins).
//
// SCOPE: the tire model below calls libm transcendentals (sin/atan/atan2/
// sqrt), which are NOT guaranteed bit-identical across different libm
// implementations. So replay is reproducible within one platform/toolchain,
// but full cross-platform lockstep determinism additionally needs a
// deterministic transcendental layer — a follow-up shared with physics-core
// (cf. Jolt's CROSS_PLATFORM_DETERMINISTIC). See the PR notes.

#pragma once

// PublicPhysicsCore.h must precede PublicVehicle.h: the vehicle header names
// ::psynder::physics::World, whose namespace is established there.
#include "physics/core/PublicPhysicsCore.h"
#include "physics/vehicle/PublicVehicle.h"

#include <cmath>
#include <cstdint>

namespace psynder::physics::vehicle {

// ─── Minimal deterministic linear algebra ────────────────────────────────────

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }

inline Vec3 normalize(Vec3 v) {
    const float len = length(v);
    return len > 0.0f ? v * (1.0f / len) : v;
}

inline Quat quat_mul(Quat a, Quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

inline Quat quat_normalize(Quat q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 0.0f) return {0.0f, 0.0f, 0.0f, 1.0f};
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Rotate v by the (assumed unit) quaternion q:  v' = q * v * q^-1, evaluated
// via the standard t = 2*(q_xyz x v); v' = v + q_w*t + q_xyz x t.
inline Vec3 quat_rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

// Integrate orientation by a world-frame angular velocity (rad/s) over dt:
// q_dot = 0.5 * omega_quat * q, then renormalize.
inline Quat quat_integrate(Quat q, Vec3 omega, float dt) {
    const Quat omega_q{omega.x, omega.y, omega.z, 0.0f};
    const Quat dq = quat_mul(omega_q, q);
    const float h = 0.5f * dt;
    return quat_normalize({q.x + dq.x * h, q.y + dq.y * h, q.z + dq.z * h, q.w + dq.w * h});
}

// ─── Physical constants ───────────────────────────────────────────────────────

inline constexpr float kGravity = 9.81f;       // m/s^2, magnitude (DESIGN §10.1)
inline constexpr float kAirDensity = 1.204f;    // kg/m^3 @ 20 C, sea level

// Pacejka-lite Magic Formula coefficients (verbatim from the lane-16 stub).
inline constexpr float kPacejkaLongB = 11.0f;
inline constexpr float kPacejkaLongC = 1.65f;
inline constexpr float kPacejkaLongE = 0.89f;
inline constexpr float kPacejkaLatB = 10.0f;
inline constexpr float kPacejkaLatC = 1.9f;
inline constexpr float kPacejkaLatE = 0.97f;

// ─── Pacejka-lite tire model ──────────────────────────────────────────────────

// The Magic Formula returns a force coefficient (force / Fz) for a given slip
// (slip ratio kappa for longitudinal, slip angle in radians for lateral). The
// peak occurs near +/- D = mu_peak; large slip rolls off past the peak.
inline float pacejka(float stiffness_b, float shape_c, float peak_d, float curvature_e,
                     float slip) {
    const float bx = stiffness_b * slip;
    return peak_d * std::sin(shape_c * std::atan(bx - curvature_e * (bx - std::atan(bx))));
}

inline float tire_mu_long(float slip_ratio, float mu_peak) {
    return pacejka(kPacejkaLongB, kPacejkaLongC, mu_peak, kPacejkaLongE, slip_ratio);
}

inline float tire_mu_lat(float slip_angle_rad, float mu_peak) {
    return pacejka(kPacejkaLatB, kPacejkaLatC, mu_peak, kPacejkaLatE, slip_angle_rad);
}

// Longitudinal slip ratio kappa = (omega*r - v_long) / max(|v_long|, eps).
// The eps floor keeps launch (v_long ~ 0) finite; a spinning wheel against a
// stationary contact yields large kappa, which Pacejka saturates near mu_peak.
inline float slip_ratio(float wheel_surface_speed, float contact_speed_long, float eps) {
    const float denom = std::fabs(contact_speed_long);
    return (wheel_surface_speed - contact_speed_long) / (denom > eps ? denom : eps);
}

inline float slip_angle(float contact_speed_long, float contact_speed_lat, float eps) {
    return std::atan2(contact_speed_lat, std::fabs(contact_speed_long) + eps);
}

// Combined-slip limiter (friction circle): the resultant of the longitudinal
// and lateral tire forces cannot exceed mu*Fz. When the pure-slip demand
// exceeds the circle both components scale down together — this is what makes
// the tires break away under simultaneous hard throttle + cornering.
inline void clamp_friction_circle(float& force_long, float& force_lat, float friction_limit) {
    if (friction_limit <= 0.0f) {
        force_long = 0.0f;
        force_lat = 0.0f;
        return;
    }
    const float mag2 = force_long * force_long + force_lat * force_lat;
    const float lim2 = friction_limit * friction_limit;
    if (mag2 > lim2) {
        const float scale = friction_limit / std::sqrt(mag2);
        force_long *= scale;
        force_lat *= scale;
    }
}

// ─── Raycast suspension ───────────────────────────────────────────────────────

inline constexpr float kBumpStopFactor = 8.0f;  // extra rate multiplier past jounce

// Normal load along the suspension axis. compression > 0 means the spring is
// compressed past its rest length; comp_rate is d(compression)/dt for damping.
// A progressive bump-stop engages past max_compression so an under-sprung
// chassis cannot sink through the travel limit. Never returns a pulling force.
inline float suspension_force(float stiffness, float damping, float compression,
                              float comp_rate, float max_compression) {
    float force = stiffness * compression + damping * comp_rate;
    if (compression > max_compression) {
        force += stiffness * kBumpStopFactor * (compression - max_compression);
    }
    return force > 0.0f ? force : 0.0f;
}

// ─── Aerodynamics ─────────────────────────────────────────────────────────────

inline float aero_drag_force(float drag_cd, float frontal_area_m2, float speed_mps) {
    return 0.5f * kAirDensity * drag_cd * frontal_area_m2 * speed_mps * speed_mps;
}

inline float aero_downforce(float lift_area_m2, float speed_mps) {
    return 0.5f * kAirDensity * lift_area_m2 * speed_mps * speed_mps;
}

// ─── Drivetrain ───────────────────────────────────────────────────────────────

// Piecewise-linear lookup on the engine torque curve (sorted ascending by
// rpm), clamped to the endpoints outside the sampled range.
inline float engine_torque_at(const EngineCurvePoint* curve, std::uint32_t count, float rpm) {
    if (curve == nullptr || count == 0) return 0.0f;
    if (rpm <= curve[0].rpm) return curve[0].torque_nm;
    if (rpm >= curve[count - 1].rpm) return curve[count - 1].torque_nm;
    for (std::uint32_t i = 1; i < count; ++i) {
        if (rpm <= curve[i].rpm) {
            const float span = curve[i].rpm - curve[i - 1].rpm;
            if (span <= 0.0f) return curve[i].torque_nm;
            const float t = (rpm - curve[i - 1].rpm) / span;
            return curve[i - 1].torque_nm + t * (curve[i].torque_nm - curve[i - 1].torque_nm);
        }
    }
    return curve[count - 1].torque_nm;
}

inline constexpr float kRadPerSecToRpm = 60.0f / (2.0f * 3.14159265358979323846f);
inline constexpr float kRpmToRadPerSec = 1.0f / kRadPerSecToRpm;

inline float rad_per_sec_to_rpm(float omega) { return std::fabs(omega) * kRadPerSecToRpm; }

}  // namespace psynder::physics::vehicle
