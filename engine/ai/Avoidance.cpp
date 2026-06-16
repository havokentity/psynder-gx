// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Avoidance.cpp — see Avoidance.h.
//
// ─── time_to_collision (the quadratic) ───────────────────────────────────────
// Work in the frame of agent `a`. Let
//   p = b.pos - a.pos      (relative position, XZ)
//   v = b.vel - a.vel      (relative velocity, XZ)
//   R = a.radius + b.radius (combined radii — the discs touch at separation R)
// The signed separation at time t is |p + v t|; the discs touch when
//   |p + v t|^2 == R^2
//   => (v·v) t^2 + 2 (p·v) t + (p·p - R^2) == 0
// a quadratic  A t^2 + B t + C == 0  with
//   A = v·v,  B = 2 (p·v),  C = p·p - R^2.
// Cases:
//   * C <= 0  -> already at/within contact at t == 0: report -1 (overlap).
//   * A <= 0  -> no relative motion (v == 0) and not overlapping: never touch.
//   * disc = B^2 - 4AC < 0 -> the path misses entirely: never touch.
//   * else the first root  t = (-B - sqrt(disc)) / (2A). If t < 0 the contact
//     is in the past (they are separating) -> never (in the future): sentinel.
//   * else t is the future contact time.
// One guarded sqrt; pure +,-,*,/; no trig/RNG -> bit-deterministic.
//
// ─── avoid_velocity (the repulsion) ──────────────────────────────────────────
// For each neighbour with a contact time inside the horizon (or already
// overlapping) we add a steering push. Two ingredients, both on XZ:
//   * separation: a unit vector from the neighbour toward self (self.pos -
//     n.pos) — shove the agents directly apart. Dominant when overlapping.
//   * perpendicular: take the component of that away-vector perpendicular to the
//     desired heading and bias along it, so the agent slides AROUND the obstacle
//     instead of only braking head-on (head-on closes have a near-zero away
//     component along the lateral, so we synthesise a lateral from the relative
//     velocity to guarantee a non-zero veer).
// Each push is scaled by an URGENCY weight that grows as the contact time shrinks
// toward 0 (imminent) and as the present gap shrinks (close). The pushes are
// summed in span order, scaled to the agent's speed budget, added to the desired
// velocity, and the whole result is clamped to max_speed.

#include "ai/Avoidance.h"

#include <cmath>

namespace psynder::ai {

namespace {

// XZ squared length.
inline f32 len2_xz(f32 x, f32 z) noexcept { return x * x + z * z; }

// Guarded XZ normalize of (x,z): returns {ux, uz}; (0,0) when degenerate.
inline void normalize_xz(f32 x, f32 z, f32& ux, f32& uz) noexcept {
    const f32 l2 = len2_xz(x, z);
    if (l2 <= 0.0f) { ux = 0.0f; uz = 0.0f; return; }
    const f32 inv = 1.0f / std::sqrt(l2);  // one guarded sqrt
    ux = x * inv;
    uz = z * inv;
}

// Build an XZ velocity {x,0,z} capped at `max_speed` (preserving direction).
// Under the cap (or stationary) it passes through; only then do we pay a sqrt.
inline math::Vec3 clamp_to_speed(f32 x, f32 z, f32 max_speed) noexcept {
    const f32 l2 = len2_xz(x, z);
    const f32 cap2 = max_speed * max_speed;
    if (l2 <= cap2 || l2 <= 0.0f) return math::Vec3{x, 0.0f, z};
    const f32 scale = max_speed / std::sqrt(l2);  // one guarded sqrt
    return math::Vec3{x * scale, 0.0f, z * scale};
}

}  // namespace

f32 time_to_collision(const AvoidAgent& a, const AvoidAgent& b) noexcept {
    // Relative position / velocity on XZ (y ignored).
    const f32 px = b.pos.x - a.pos.x;
    const f32 pz = b.pos.z - a.pos.z;
    const f32 vx = b.vel.x - a.vel.x;
    const f32 vz = b.vel.z - a.vel.z;

    const f32 R = a.radius + b.radius;
    const f32 pp = len2_xz(px, pz);
    const f32 C = pp - R * R;

    // Already touching / interpenetrating at t == 0.
    if (C <= 0.0f) return -1.0f;

    const f32 A = len2_xz(vx, vz);       // v·v
    if (A <= 0.0f) return kNoCollision;  // no relative motion, not overlapping

    const f32 b_half = px * vx + pz * vz;  // p·v  (== B/2)
    // If p·v >= 0 they are not closing (the gap is non-decreasing) -> no future
    // contact. (Catches the separating case before the sqrt.)
    if (b_half >= 0.0f) return kNoCollision;

    // Discriminant of A t^2 + 2 b_half t + C, using the halved-B form:
    //   disc/4 = b_half^2 - A C.
    const f32 disc_q = b_half * b_half - A * C;
    if (disc_q < 0.0f) return kNoCollision;  // path misses the disc entirely

    // First (smallest) root with the halved-B quadratic formula:
    //   t = (-b_half - sqrt(disc_q)) / A.
    const f32 t = (-b_half - std::sqrt(disc_q)) / A;  // one guarded sqrt
    if (t < 0.0f) return kNoCollision;  // contact only in the past -> separating
    return t;
}

math::Vec3 avoid_velocity(const AvoidAgent& self, math::Vec3 desired_vel,
                          std::span<const AvoidAgent> neighbours,
                          f32 time_horizon_s, f32 max_speed) noexcept {
    // Desired heading on XZ (guarded). Used to split the away-vector into a
    // lateral (perpendicular) component so we slide AROUND rather than only brake.
    f32 hx = 0.0f, hz = 0.0f;
    normalize_xz(desired_vel.x, desired_vel.z, hx, hz);

    // Accumulated avoidance push (XZ), summed over neighbours in span order.
    f32 ax = 0.0f, az = 0.0f;

    const f32 horizon = time_horizon_s > 0.0f ? time_horizon_s : 0.0f;

    for (const AvoidAgent& n : neighbours) {
        const f32 ttc = time_to_collision(self, n);

        // Threat test: imminent future contact within the horizon, or an
        // already-overlapping pair (ttc == -1). Anything else (separating /
        // never / contact beyond the horizon) is ignored.
        const bool overlapping = ttc < 0.0f;
        const bool imminent = ttc >= 0.0f && ttc <= horizon;
        if (!overlapping && !imminent) continue;

        // Present gap and away-direction (from neighbour toward self) on XZ.
        const f32 dx = self.pos.x - n.pos.x;
        const f32 dz = self.pos.z - n.pos.z;
        const f32 gap2 = len2_xz(dx, dz);
        const f32 R = self.radius + n.radius;

        // Unit away-vector (separation direction). When centres coincide we fall
        // back to the reverse desired heading so we still get a defined push.
        f32 awx = 0.0f, awz = 0.0f;
        normalize_xz(dx, dz, awx, awz);
        if (awx == 0.0f && awz == 0.0f) { awx = -hx; awz = -hz; }

        // Lateral (perpendicular-to-heading) component of the away-vector: this
        // is the "go around" direction. perp = away - (away·h) h.
        const f32 a_dot_h = awx * hx + awz * hz;
        f32 perpx = awx - a_dot_h * hx;
        f32 perpz = awz - a_dot_h * hz;
        f32 plx = 0.0f, plz = 0.0f;
        normalize_xz(perpx, perpz, plx, plz);
        if (plx == 0.0f && plz == 0.0f) {
            // Head-on (away nearly anti-parallel to heading): the away-vector has
            // no lateral part. Synthesise a deterministic sideways direction from
            // the heading by rotating it 90 deg on XZ: (hx,hz) -> (hz,-hx).
            plx = hz;
            plz = -hx;
        }

        // ── Urgency weight ──────────────────────────────────────────────────
        // Time urgency: 1 when contact is now (ttc == 0), ramping to 0 at the
        // horizon. Overlapping pairs get the full 1. Distance urgency: grows as
        // the present gap closes toward the combined radii.
        f32 time_w;
        if (overlapping) {
            time_w = 1.0f;
        } else if (horizon > 0.0f) {
            time_w = (horizon - ttc) / horizon;   // in [0, 1]
            if (time_w < 0.0f) time_w = 0.0f;
        } else {
            time_w = 1.0f;
        }

        // Distance weight: R / gap, clamped to [1, kCloseCap]. Far -> ~ R/gap
        // (small); close -> large; capped so an exact overlap can't blow up.
        constexpr f32 kCloseCap = 8.0f;
        f32 dist_w = 1.0f;
        if (gap2 > 0.0f) {
            // R^2 / gap^2 avoids a sqrt; it has the same monotonic behaviour.
            dist_w = (R * R) / gap2;
            if (dist_w < 1.0f) dist_w = 1.0f;
            else if (dist_w > kCloseCap) dist_w = kCloseCap;
        } else {
            dist_w = kCloseCap;  // coincident centres -> max push
        }

        const f32 urgency = time_w * dist_w;

        // Perpendicular bias (slide around) + separation (push apart). When
        // overlapping we lean harder on raw separation to resolve the overlap.
        constexpr f32 kPerpBias = 1.0f;
        const f32 sep_bias = overlapping ? 1.5f : 0.6f;

        ax += urgency * (kPerpBias * plx + sep_bias * awx);
        az += urgency * (kPerpBias * plz + sep_bias * awz);
    }

    // No threats touched the accumulator: pure goal pursuit, just clamped.
    if (ax == 0.0f && az == 0.0f) {
        return clamp_to_speed(desired_vel.x, desired_vel.z, max_speed);
    }

    // Scale the avoidance to the speed budget so it can actually bend the path,
    // then add it to the desired velocity and clamp the whole thing.
    const f32 push = max_speed > 0.0f ? max_speed : 1.0f;
    const f32 rx = desired_vel.x + ax * push;
    const f32 rz = desired_vel.z + az * push;
    return clamp_to_speed(rx, rz, max_speed);
}

}  // namespace psynder::ai
