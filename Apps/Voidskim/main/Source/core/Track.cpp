#include "Track.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hd {

// Centreline sample spacing, in metres
constexpr float SAMPLE_SPACING = 4.0f;
// Limits that keep a track's numbers sane (and its sample count castable and allocatable)
constexpr float MAX_SEGMENT_LENGTH = 5000.0f; // metres
constexpr size_t MAX_SAMPLES = 5000; // a 20 km lap

bool parseColour(const char* text, uint32_t& out) {
    if (text == nullptr) {
        return false;
    }
    if (text[0] == '#') {
        text++;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    if (strlen(text) != 6) {
        return false;
    }
    char* end = nullptr;
    const unsigned long value = strtoul(text, &end, 16);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = (uint32_t)value;
    return true;
}

namespace {

struct BuildState {
    Vec2 position;
    float heading = 0.0f;
    float distance = 0.0f;
    float width = 16.0f; // current width
    float targetWidth = 16.0f; // reached at the end of the next segment
    bool wallLeft = true;
    bool wallRight = true;
    float baseHeight = 0.0f; // road height
    float targetHeight = 0.0f; // reached at the end of the next segment ("height")
    bool tunnel = false;
};

// Smooth 0..1 easing for climbs, so slopes start and end gently
float smoothstep(float u) { return u * u * (3.0f - 2.0f * u); }

bool parseFloat(const char* text, float& out) {
    if (text == nullptr) {
        return false;
    }
    char* end = nullptr;
    out = strtof(text, &end);
    // strtof also reads "inf" and "nan" (and overflows to infinity): only finite numbers count
    return end != text && *end == '\0' && std::isfinite(out);
}

} // namespace

bool Track::load(const std::string& text, std::string& error, const char* difficulty) {
    samples_.clear();
    name_ = "Track";
    laps_ = 3;
    theme_ = TrackTheme();

    BuildState state;
    bool widthSet = false;
    bool steepError = false;
    const char* segmentError = nullptr;

    // Appends the samples of one segment. heightAt(u) gives the surface height at u = 0..1.
    auto addSegment = [&](float segmentLength, float turnRadians, bool gap, bool ramp, float rampHeight) {
        // Checked before the sample count is worked out (and cast to int)
        if (!std::isfinite(segmentLength) || segmentLength <= 0.0f || segmentLength > MAX_SEGMENT_LENGTH) {
            segmentError = "segment is too long (at most 5000 m)";
            return;
        }
        if (samples_.size() + (size_t)(segmentLength / SAMPLE_SPACING) + 2 > MAX_SAMPLES) {
            segmentError = "track is too long (at most 20 km)";
            return;
        }
        // A "height" change eases in over this segment
        const float climb = state.targetHeight - state.baseHeight;
        if (std::fabs(climb) > segmentLength * 0.3f) {
            steepError = true;
        }
        const int steps = segmentLength <= SAMPLE_SPACING ? 1 : (int)std::ceil(segmentLength / SAMPLE_SPACING);
        const float stepLength = segmentLength / (float)steps;
        const float stepTurn = turnRadians / (float)steps;
        const float startWidth = state.width;
        for (int i = 0; i < steps; i++) {
            const float u = (float)i / (float)steps;
            TrackSample sample;
            sample.position = state.position;
            sample.distance = state.distance;
            sample.halfWidth = lerpf(startWidth, state.targetWidth, u) * 0.5f;
            sample.gap = gap;
            sample.ramp = ramp;
            sample.base = state.baseHeight + climb * smoothstep(u);
            sample.height = ramp ? sample.base + rampHeight * u : sample.base;
            sample.tunnel = state.tunnel && !gap;
            // A tunnel always has its walls
            sample.wallLeft = (state.wallLeft || sample.tunnel) && !gap;
            sample.wallRight = (state.wallRight || sample.tunnel) && !gap;
            samples_.push_back(sample);

            // Arcs are stepped by turning half before and half after each chord, so a constant
            // radius curve stays accurate however coarse the sampling
            state.heading += stepTurn * 0.5f;
            state.position += headingToDirection(state.heading) * stepLength;
            state.heading += stepTurn * 0.5f;
            state.distance += stepLength;
        }
        state.width = state.targetWidth;
        state.baseHeight += climb;
        if (ramp) {
            // The top of the ramp, where the road drops off
            TrackSample top;
            top.position = state.position;
            top.distance = state.distance;
            top.halfWidth = state.width * 0.5f;
            top.base = state.baseHeight;
            top.height = state.baseHeight + rampHeight;
            top.ramp = true;
            top.rampTop = true;
            top.wallLeft = state.wallLeft;
            top.wallRight = state.wallRight;
            samples_.push_back(top);
            // Nudged forward so the next segment doesn't start on the exact same spot
            state.position += headingToDirection(state.heading) * 0.05f;
            state.distance += 0.05f;
        }
    };

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

        // Comments: '#' at the start of a line, or '#' followed by whitespace. A "#RRGGBB" colour
        // value is neither.
        const size_t firstText = line.find_first_not_of(" \t");
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '#' && (i == firstText || i + 1 == line.size() || line[i + 1] == ' ' || line[i + 1] == '\t')) {
                line.resize(i);
                break;
            }
        }

        std::vector<char> buffer(line.begin(), line.end());
        buffer.push_back('\0');
        const char* tokens[8] = {};
        int count = 0;
        for (char* token = strtok(buffer.data(), " \t\r"); token != nullptr && count < 8; token = strtok(nullptr, " \t\r")) {
            tokens[count++] = token;
        }
        if (count == 0) {
            continue;
        }

        const std::string command = tokens[0];
        char message[96];
        auto fail = [&](const char* what) {
            snprintf(message, sizeof(message), "line %d: %s", lineNumber, what);
            error = message;
            return false;
        };
        float a = 0.0f;
        float b = 0.0f;

        if (command == "name") {
            name_ = line.substr(line.find("name") + 4);
            while (!name_.empty() && (name_.front() == ' ' || name_.front() == '\t')) name_.erase(0, 1);
            while (!name_.empty() && (name_.back() == ' ' || name_.back() == '\r')) name_.pop_back();
        } else if (command == "laps") {
            if (count < 2 || !parseFloat(tokens[1], a) || a < 1 || a > 20) return fail("laps needs a number from 1 to 20");
            laps_ = (int)a;
        } else if (command == "width") {
            if (count < 2 || !parseFloat(tokens[1], a) || a < 6 || a > 60) return fail("width needs a number from 6 to 60");
            state.targetWidth = a;
            if (!widthSet && samples_.empty()) {
                state.width = a; // the first width applies from the start
            }
            widthSet = true;
        } else if (command == "walls") {
            if (count < 2) return fail("walls needs both, left, right or none");
            // Optional difficulty names after it: the line only applies on those difficulties
            if (count > 2) {
                bool applies = false;
                for (int t = 2; t < count; t++) {
                    if (difficulty != nullptr && strcmp(tokens[t], difficulty) == 0) applies = true;
                    else if (strcmp(tokens[t], "easy") != 0 && strcmp(tokens[t], "normal") != 0 && strcmp(tokens[t], "hard") != 0) {
                        return fail("walls difficulties must be easy, normal or hard");
                    }
                }
                if (!applies) continue;
            }
            const std::string which = tokens[1];
            if (which == "both") { state.wallLeft = true; state.wallRight = true; }
            else if (which == "left") { state.wallLeft = true; state.wallRight = false; }
            else if (which == "right") { state.wallLeft = false; state.wallRight = true; }
            else if (which == "none") { state.wallLeft = false; state.wallRight = false; }
            else return fail("walls needs both, left, right or none");
        } else if (command == "straight") {
            if (count < 2 || !parseFloat(tokens[1], a) || a <= 0) return fail("straight needs a length");
            addSegment(a, 0.0f, false, false, 0.0f);
        } else if (command == "curve") {
            if (count < 3 || !parseFloat(tokens[1], a) || !parseFloat(tokens[2], b) || b < 5) {
                return fail("curve needs an angle and a radius of at least 5");
            }
            addSegment(std::fabs(a) * DEG_TO_RAD * b, a * DEG_TO_RAD, false, false, 0.0f);
        } else if (command == "ramp") {
            if (count < 3 || !parseFloat(tokens[1], a) || !parseFloat(tokens[2], b) || a <= 0 || b <= 0) {
                return fail("ramp needs a length and a height");
            }
            addSegment(a, 0.0f, false, true, b);
        } else if (command == "height") {
            if (count < 2 || !parseFloat(tokens[1], a) || a < 0.0f || a > 40.0f) return fail("height needs a number from 0 to 40");
            state.targetHeight = a;
        } else if (command == "tunnel") {
            if (count < 2) return fail("tunnel needs on or off");
            const std::string which = tokens[1];
            if (which == "on") state.tunnel = true;
            else if (which == "off") state.tunnel = false;
            else return fail("tunnel needs on or off");
        } else if (command == "gap") {
            if (count < 2 || !parseFloat(tokens[1], a) || a <= 0) return fail("gap needs a length");
            addSegment(a, 0.0f, true, false, 0.0f);
        } else if (command == "colour" || command == "color") {
            uint32_t colour = 0;
            if (count < 3 || !parseColour(tokens[2], colour)) return fail("colour needs a name and #RRGGBB");
            const std::string key = tokens[1];
            if (key == "sky") theme_.skyTop = colour;
            else if (key == "horizon") theme_.skyHorizon = colour;
            else if (key == "below") theme_.below = colour;
            else if (key == "road") theme_.roadA = colour;
            else if (key == "road2") theme_.roadB = colour;
            else if (key == "edge") theme_.edgeA = colour;
            else if (key == "edge2") theme_.edgeB = colour;
            else if (key == "wall") theme_.wall = colour;
            else if (key == "ramp") theme_.ramp = colour;
            else if (key == "structure") theme_.structure = colour;
            else if (key == "scenery") theme_.scenery = colour;
            else return fail("unknown colour name");
        } else {
            return fail("unknown command");
        }
        if (segmentError != nullptr) {
            return fail(segmentError);
        }
        if (steepError) {
            return fail("height changes too steeply for this segment (at most 0.3 m per metre)");
        }
    }

    if (samples_.size() < 8) {
        error = "track is too short";
        return false;
    }

    if (std::fabs(state.baseHeight) > 0.05f) {
        char message[96];
        snprintf(message, sizeof(message), "track doesn't end at the height it started (%.1f m off): end at height 0", state.baseHeight);
        error = message;
        return false;
    }

    // Close the loop: spread the remaining offset back to the start over the whole lap
    const Vec2 closingError = state.position - samples_.front().position;
    const float totalDistance = state.distance;
    if (hd::length(closingError) > totalDistance * 0.1f) {
        char message[96];
        snprintf(message, sizeof(message), "track doesn't end near its start (%.0f m off)", hd::length(closingError));
        error = message;
        return false;
    }
    for (auto& sample : samples_) {
        sample.position -= closingError * (sample.distance / totalDistance);
    }
    length_ = totalDistance;

    return finish(error);
}

bool Track::finish(std::string& error) {
    const int n = (int)samples_.size();
    // Directions from the (corrected) neighbours, so corners stay smooth after closing the loop
    for (int i = 0; i < n; i++) {
        const Vec2 forward = sample(i + 1).position - sample(i - 1).position;
        samples_[i].direction = normalised(forward);
        samples_[i].right = rightOf(samples_[i].direction);
    }
    // Recompute distances along the corrected centreline
    float distance = 0.0f;
    for (int i = 0; i < n; i++) {
        samples_[i].distance = distance;
        distance += hd::length(sample(i + 1).position - samples_[i].position);
    }
    length_ = distance;

    // Where the track crosses itself: parts of the lap far apart (in distance) that overlap on the
    // ground. One has to pass over the other with room to spare (a bridge, usually over a tunnel):
    // crossing at the same level isn't allowed.
    constexpr float MIN_CLEARANCE = 7.0f;
    for (int i = 0; i < n; i++) {
        TrackSample& a = samples_[i];
        for (int j = 0; j < n; j++) {
            const TrackSample& b = samples_[j];
            const float along = std::fabs(a.distance - b.distance);
            if (std::fmin(along, length_ - along) < 40.0f) continue; // the same stretch of road
            const float reach = a.halfWidth + b.halfWidth;
            if (lengthSquared(a.position - b.position) > reach * reach) continue;
            if (std::fabs(a.height - b.height) < MIN_CLEARANCE) {
                char message[128];
                snprintf(message, sizeof(message),
                    "the track crosses itself %.0f m from the start without a bridge: use height to raise one part at least %.0f m",
                    a.distance, MIN_CLEARANCE);
                error = message;
                return false;
            }
            if (a.height > b.height) {
                a.overpass = true;
            }
        }
    }

    curvature_.assign(n, 0.0f);
    for (int i = 0; i < n; i++) {
        const float turn = wrapAngle(directionToHeading(sample(i + 1).direction) - directionToHeading(sample(i - 1).direction));
        const float span = hd::length(sample(i + 1).position - sample(i - 1).position);
        curvature_[i] = span > 0.01f ? turn / span : 0.0f;
    }
    return true;
}

TrackLocation Track::locate(const Vec2& point, int hint) const {
    const int n = (int)samples_.size();
    int best = 0;
    float bestDistance2 = 1e30f;
    float bestT = 0.0f;

    auto test = [&](int k) {
        const TrackSample& a = sample(k);
        const TrackSample& b = sample(k + 1);
        const Vec2 ab = b.position - a.position;
        const float len2 = lengthSquared(ab);
        float t = len2 > 1e-6f ? dot(point - a.position, ab) / len2 : 0.0f;
        t = clampf(t, 0.0f, 1.0f);
        const Vec2 closest = a.position + ab * t;
        const float d2 = lengthSquared(point - closest);
        if (d2 < bestDistance2) {
            bestDistance2 = d2;
            best = k;
            bestT = t;
        }
    };

    if (hint >= 0) {
        for (int k = hint - 4; k <= hint + 12; k++) {
            test(wrap(k));
        }
        // Lost track (e.g. respawned or teleported): fall back to a full search
        const float halfWidth = sample(best).halfWidth;
        if (bestDistance2 > (halfWidth * 4.0f) * (halfWidth * 4.0f)) {
            hint = -1;
        }
    }
    if (hint < 0) {
        bestDistance2 = 1e30f;
        for (int k = 0; k < n; k++) {
            test(k);
        }
    }

    const TrackSample& a = sample(best);
    const TrackSample& b = sample(best + 1);
    TrackLocation location;
    location.segment = best;
    location.t = bestT;
    location.centre = a.position + (b.position - a.position) * bestT;
    location.direction = normalised(a.direction * (1.0f - bestT) + b.direction * bestT);
    location.lateral = dot(point - location.centre, rightOf(location.direction));
    location.halfWidth = lerpf(a.halfWidth, b.halfWidth, bestT);
    const float segmentLength = hd::length(b.position - a.position);
    location.distance = a.distance + segmentLength * bestT;
    if (location.distance >= length_) {
        location.distance -= length_;
    }
    location.wallLeft = a.wallLeft;
    location.wallRight = a.wallRight;
    location.tunnel = a.tunnel;
    location.hasSurface = !a.gap && std::fabs(location.lateral) <= location.halfWidth;
    // After the top of a ramp the road is back at ground level: a sheer drop
    location.surfaceHeight = a.rampTop ? b.height : lerpf(a.height, b.height, bestT);
    return location;
}

void Track::pointAt(float distance, Vec2& outPosition, Vec2& outDirection, int* outSegment) const {
    distance = std::fmod(distance, length_);
    if (distance < 0.0f) {
        distance += length_;
    }
    // Binary search for the segment containing the distance
    int lo = 0;
    int hi = (int)samples_.size() - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (samples_[mid].distance <= distance) lo = mid; else hi = mid - 1;
    }
    const TrackSample& a = samples_[lo];
    const TrackSample& b = sample(lo + 1);
    const float segmentLength = hd::length(b.position - a.position);
    const float t = segmentLength > 0.001f ? clampf((distance - a.distance) / segmentLength, 0.0f, 1.0f) : 0.0f;
    outPosition = a.position + (b.position - a.position) * t;
    outDirection = normalised(a.direction * (1.0f - t) + b.direction * t);
    if (outSegment != nullptr) {
        *outSegment = lo;
    }
}

float Track::curvatureAhead(float distance, float span) const {
    Vec2 position;
    Vec2 direction;
    int segment = 0;
    pointAt(distance, position, direction, &segment);
    const int steps = (int)(span / SAMPLE_SPACING) + 1;
    float strongest = 0.0f;
    for (int i = 0; i < steps; i++) {
        const float c = curvature_[wrap(segment + i)];
        if (std::fabs(c) > std::fabs(strongest)) {
            strongest = c;
        }
    }
    return strongest;
}

int Track::safeSampleAtOrBefore(int segment) const {
    const int n = (int)samples_.size();
    for (int back = 0; back < n; back++) {
        const int k = wrap(segment - back);
        bool safe = true;
        for (int j = -3; j <= 3 && safe; j++) {
            const TrackSample& s = sample(k + j);
            safe = !s.gap && !s.ramp;
        }
        if (safe) {
            return k;
        }
    }
    return 0;
}

float Track::delta(float from, float to) const {
    float d = to - from;
    if (d > length_ * 0.5f) d -= length_;
    if (d < -length_ * 0.5f) d += length_;
    return d;
}

void Track::bounds(Vec2& outMin, Vec2& outMax) const {
    outMin = { 1e30f, 1e30f };
    outMax = { -1e30f, -1e30f };
    for (const auto& s : samples_) {
        outMin.x = std::fmin(outMin.x, s.position.x);
        outMin.z = std::fmin(outMin.z, s.position.z);
        outMax.x = std::fmax(outMax.x, s.position.x);
        outMax.z = std::fmax(outMax.z, s.position.z);
    }
}

} // namespace hd
