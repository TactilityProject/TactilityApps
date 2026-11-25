#include "Vehicle.h"

#include "Track.h" // parseColour

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hd {

bool parseVehicles(const std::string& text, std::vector<VehicleSpec>& out, std::string& error) {
    out.clear();
    int lineNumber = 0;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = text.size();
        }
        std::string line = text.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        lineNumber++;

        // Comments: '#' at the start of a line, or '#' followed by whitespace
        const size_t firstText = line.find_first_not_of(" \t");
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '#' && (i == firstText || i + 1 == line.size() || line[i + 1] == ' ' || line[i + 1] == '\t')) {
                line.resize(i);
                break;
            }
        }

        std::vector<char> buffer(line.begin(), line.end());
        buffer.push_back('\0');
        char* key = strtok(buffer.data(), " \t\r");
        if (key == nullptr) {
            continue;
        }
        char* value = strtok(nullptr, "\r");
        while (value != nullptr && (*value == ' ' || *value == '\t')) {
            value++;
        }

        char message[96];
        auto fail = [&](const char* what) {
            snprintf(message, sizeof(message), "line %d: %s", lineNumber, what);
            error = message;
            return false;
        };
        if (value == nullptr || *value == '\0') {
            return fail("missing value");
        }

        const std::string name = key;
        if (name == "vehicle") {
            out.emplace_back();
            out.back().name = value;
            continue;
        }
        if (out.empty()) {
            return fail("expected 'vehicle <name>' first");
        }
        VehicleSpec& v = out.back();

        if (name == "shape") {
            const std::string shape = value;
            if (shape == "dart") v.shape = CraftShape::Dart;
            else if (shape == "wing") v.shape = CraftShape::Wing;
            else if (shape == "hauler") v.shape = CraftShape::Hauler;
            else return fail("shape must be dart, wing or hauler");
            continue;
        }
        if (name == "colour" || name == "color" || name == "accent") {
            uint32_t colour = 0;
            if (!parseColour(value, colour)) return fail("colour must be #RRGGBB");
            (name == "accent" ? v.accent : v.colour) = colour;
            continue;
        }

        char* end = nullptr;
        const float number = strtof(value, &end);
        // strtof accepts "inf" and "nan", which would break the physics: only finite values
        if (end == value || !std::isfinite(number) || number < 0.0f) {
            return fail("expected a positive number");
        }
        if (name == "speed") v.topSpeed = number;
        else if (name == "accel") v.acceleration = number;
        else if (name == "brake") v.braking = number;
        else if (name == "turn") v.turnRate = number;
        else if (name == "grip") v.grip = number;
        else if (name == "boost") v.boostSpeed = number;
        else if (name == "boosttime") v.boostTime = number > 0.1f ? number : 0.1f;
        else if (name == "recharge") v.boostRecharge = number;
        else if (name == "mass") v.mass = number > 0.1f ? number : 0.1f;
        else if (name == "air") v.airControl = number > 1.0f ? 1.0f : number;
        else return fail("unknown property");
    }
    if (out.empty()) {
        error = "no vehicles defined";
        return false;
    }
    return true;
}

std::vector<VehicleSpec> defaultVehicles() {
    // Keep in sync with assets/vehicles.txt
    std::vector<VehicleSpec> list(3);
    list[0].name = "Arrow";
    list[0].shape = CraftShape::Dart;
    list[0].colour = 0xE63946;
    list[0].accent = 0xF1FAEE;
    list[0].topSpeed = 69.0f;
    list[0].acceleration = 22.4f;
    list[0].braking = 45.0f;
    list[0].turnRate = 1.9f;
    list[0].grip = 4.0f;
    list[0].boostSpeed = 15.0f;
    list[0].boostTime = 2.0f;
    list[0].boostRecharge = 0.18f;
    list[0].mass = 1.0f;
    list[0].airControl = 0.3f;

    list[1].name = "Swift";
    list[1].shape = CraftShape::Wing;
    list[1].colour = 0x3A86FF;
    list[1].accent = 0xFFD60A;
    list[1].topSpeed = 66.0f;
    list[1].acceleration = 25.0f;
    list[1].braking = 50.0f;
    list[1].turnRate = 2.2f;
    list[1].grip = 5.5f;
    list[1].boostSpeed = 20.0f;
    list[1].boostTime = 2.5f;
    list[1].boostRecharge = 0.22f;
    list[1].mass = 1.0f;
    list[1].airControl = 0.4f;

    list[2].name = "Bulwark";
    list[2].shape = CraftShape::Hauler;
    list[2].colour = 0x2A9D8F;
    list[2].accent = 0xE9C46A;
    list[2].topSpeed = 64.5f;
    list[2].acceleration = 28.5f;
    list[2].braking = 40.0f;
    list[2].turnRate = 2.1f;
    list[2].grip = 2.0f;
    list[2].boostSpeed = 32.0f;
    list[2].boostTime = 4.0f;
    list[2].boostRecharge = 0.3f;
    list[2].mass = 1.8f;
    list[2].airControl = 0.25f;
    return list;
}

} // namespace hd
