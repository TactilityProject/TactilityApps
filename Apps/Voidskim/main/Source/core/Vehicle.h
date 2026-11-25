#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hd {

/** Body shape of a craft (see WorldRenderer's mesh builders). */
enum class CraftShape {
    Dart, // slim needle with twin rear engines
    Wing, // wide, flat delta
    Hauler, // heavy twin-pod body
};

/**
 * Flight characteristics of a craft. Speeds are in m/s, accelerations in m/s^2.
 *
 * Text format (vehicles.txt), one block per craft:
 *
 *   vehicle Arrow          starts a craft, with its display name
 *   shape dart             dart | wing | hauler
 *   colour #E63946         body colour
 *   accent #F1FAEE         trim colour
 *   speed 64               top speed
 *   accel 24               acceleration
 *   brake 45               braking
 *   turn 2.1               turn rate in radians per second at full lock
 *   grip 5                 how quickly sideways drift dies out (higher = stickier)
 *   boost 20               extra top speed while boosting
 *   boosttime 2.5          seconds of boost in a full tank
 *   recharge 0.22          tank fraction recovered per second
 *   mass 1.0               relative weight in collisions
 *   air 0.35               steering left while airborne (0..1)
 */
struct VehicleSpec {
    std::string name = "Craft";
    CraftShape shape = CraftShape::Dart;
    uint32_t colour = 0xE63946;
    uint32_t accent = 0xF1FAEE;
    float topSpeed = 60.0f;
    float acceleration = 22.0f;
    float braking = 40.0f;
    float turnRate = 2.0f;
    float grip = 5.0f;
    float boostSpeed = 18.0f;
    float boostTime = 2.5f;
    float boostRecharge = 0.22f;
    float mass = 1.0f;
    float airControl = 0.35f;
};

/** Parses vehicles.txt. @return false with a message in error when the text is invalid */
bool parseVehicles(const std::string& text, std::vector<VehicleSpec>& out, std::string& error);

/** The built-in crafts, used when vehicles.txt can't be read. */
std::vector<VehicleSpec> defaultVehicles();

} // namespace hd
