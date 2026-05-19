// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/vehicle/PublicVehicle.h
//
// Lane 16 — Vehicle module public-header CONTRACT (Wave 0).
//
// Raycast suspension, Pacejka-lite tires, drivetrain. Real units:
// engine torque in N·m (jeep ~250 Nm peak), tire grip µ ~ 1.0–1.2 on dry
// tarmac, masses in kg (sedan ~1200, light tank ~12000). See DESIGN §10.1.

#pragma once

#include <cstdint>

namespace psynder::physics::vehicle {

struct Vehicle;

enum class Drivetrain : std::uint8_t { Fwd, Rwd, Awd, Tracked };

struct WheelDesc {
    float local_pos[3];           // attach point in vehicle local space (m)
    float suspension_rest_m;      // rest length
    float suspension_stiffness;   // N/m
    float suspension_damping;     // N·s/m
    float tire_radius_m;
    float tire_width_m;
    float tire_friction_mu;       // 1.0–1.2 dry tarmac, ~0.6 wet, ~0.2 ice
    bool  is_driven;              // hooked to drivetrain
    bool  is_steered;             // turns under steering input
    bool  is_braked;              // bound to handbrake/brake
};

struct EngineCurvePoint { float rpm; float torque_nm; };

struct VehicleDesc {
    float mass_kg = 1200.0f;
    float drag_cd = 0.30f;
    float frontal_area_m2 = 2.0f;
    float center_of_mass_offset_z_m = -0.05f; // below geometric center
    Drivetrain drivetrain = Drivetrain::Rwd;
    std::uint32_t wheel_count = 4;
    const WheelDesc* wheels = nullptr;
    // Engine curve (sorted by rpm)
    std::uint32_t engine_curve_n = 0;
    const EngineCurvePoint* engine_curve = nullptr;
    // Gearbox ratios (forward gears first, then reverse). Real gear ratios
    // like {-3.5, 0, 3.5, 2.1, 1.4, 1.0, 0.8} for a 5-speed manual.
    std::uint32_t gear_count = 0;
    const float* gear_ratios = nullptr;
    float final_drive = 3.5f;
};

Vehicle* create_vehicle(struct ::psynder::physics::World*, const VehicleDesc&);
void     destroy_vehicle(Vehicle*);

struct VehicleInput {
    float throttle = 0.0f;  // 0..1
    float brake    = 0.0f;  // 0..1
    float steer    = 0.0f;  // -1..1
    bool  handbrake = false;
    int   gear_request = 0; // 0 = no change; positive = upshift; negative = downshift
};
void vehicle_tick(Vehicle*, const VehicleInput&, float dt);
void vehicle_get_transform(const Vehicle*, float out_pos[3], float out_quat[4]);
float vehicle_get_speed_mps(const Vehicle*);

} // namespace psynder::physics::vehicle
