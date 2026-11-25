#pragma once

#include "MathUtil.h"

#include <string>
#include <vector>

namespace hd {

/** Height of a tunnel's roof above the road, in metres. */
constexpr float TUNNEL_CLEARANCE = 6.0f;

/** Colours of a track, as 0xRRGGBB. */
struct TrackTheme {
    uint32_t skyTop = 0x1A2A6C;
    uint32_t skyHorizon = 0xF7A35C;
    uint32_t below = 0x0B0B1A; // the void under the track
    uint32_t roadA = 0x3A3F4B;
    uint32_t roadB = 0x31353F;
    uint32_t edgeA = 0xE8E8E8; // edge stripes where there's no barrier (you can fall off here)
    uint32_t edgeB = 0xD62828;
    uint32_t wall = 0x2EC4B6; // barriers
    uint32_t ramp = 0xF5C518;
    uint32_t structure = 0x4B4E6D; // the track's underside and the pillars holding it up
    uint32_t scenery = 0x8338EC; // decorative towers
};

/**
 * One cross-section of the track. The centreline is sampled every few metres; segment k is the
 * stretch from sample k to sample k + 1 (wrapping around at the end).
 */
struct TrackSample {
    Vec2 position; // centreline, in metres
    Vec2 direction; // unit forward direction
    Vec2 right; // unit direction to the right of forward
    float distance = 0.0f; // along the centreline from the start line
    float halfWidth = 8.0f;
    float height = 0.0f; // road surface height
    float base = 0.0f; // road height without the ramp (the ground a ramp stands on)
    bool gap = false; // segment starting here has no road: you need to jump it
    bool ramp = false; // segment starting here is a ramp
    bool rampTop = false; // last ramp sample: the road drops off right after it
    bool wallLeft = true; // segment starting here has a barrier on its left edge
    bool wallRight = true;
    bool tunnel = false; // segment starting here is covered (walls and a roof)
    bool overpass = false; // passes above another part of the track
};

/** Where a point is relative to the track. */
struct TrackLocation {
    int segment = 0; // sample index the point is beside
    float t = 0.0f; // 0..1 along that segment
    float distance = 0.0f; // along the centreline, 0..length
    float lateral = 0.0f; // signed distance from the centreline, positive is right
    float halfWidth = 8.0f;
    bool hasSurface = false; // there is road under the point
    float surfaceHeight = 0.0f;
    bool wallLeft = false;
    bool wallRight = false;
    bool tunnel = false;
    Vec2 direction;
    Vec2 centre;
};

/**
 * A closed circuit, built from a small text format:
 *
 *   # comment             a '#' starting a line, or followed by a space
 *   name Harbor Loop      display name
 *   laps 3                default lap count
 *   width 18              track width in metres; takes effect gradually over the next segment
 *   walls both            barriers on: both | left | right | none (none/one side: you can fall off).
 *                         Difficulty names after it (easy normal hard) apply it only on those
 *   height 8              road height in metres; eases in over the next segment, at most 0.3 m
 *                         per metre. The track has to end at the height it started
 *   tunnel on             covered road (walls and a roof) until "tunnel off"
 *   straight 120          straight, length in metres
 *   curve 90 60           curve: angle in degrees (positive turns right), radius in metres
 *   ramp 24 3             kicker ramp: length and rise in metres; you jump off its end
 *   gap 18                no road for this length: jump it or fall
 *   colour <key> <rgb>    theme colour: sky, horizon, below, road, road2, edge, edge2, wall, ramp,
 *                         structure, scenery. rgb is #RRGGBB or 0xRRGGBB
 *
 * The track's music is the .song file next to it with the same name (see Sound.h).
 *
 * The circuit should end where it started, facing the same way. Small mismatches are closed by
 * spreading the error over the whole lap.
 *
 * The track may cross itself, but only at different levels: one part has to pass at least 7 m
 * above the other (a bridge, usually over a tunnel).
 */
class Track {
public:
    /** Parses and builds a track. @return false with a message in error when the text is invalid */
    /** @param difficulty easy, normal or hard: selects difficulty-specific walls (nullptr: none) */
    bool load(const std::string& text, std::string& error, const char* difficulty = nullptr);

    const std::string& name() const { return name_; }
    int laps() const { return laps_; }
    float length() const { return length_; }
    const TrackTheme& theme() const { return theme_; }
    const std::vector<TrackSample>& samples() const { return samples_; }
    int sampleCount() const { return (int)samples_.size(); }
    const TrackSample& sample(int index) const { return samples_[wrap(index)]; }
    int wrap(int index) const {
        const int n = (int)samples_.size();
        index %= n;
        return index < 0 ? index + n : index;
    }

    /**
     * Finds where a point is on the track.
     * @param hint the segment found last time for this object, or -1 for a full search
     */
    TrackLocation locate(const Vec2& point, int hint) const;

    /** Centreline position and direction at a distance along the track (wraps). */
    void pointAt(float distance, Vec2& outPosition, Vec2& outDirection, int* outSegment = nullptr) const;

    /** Curvature (radians per metre, signed, positive turns right) averaged over a stretch ahead. */
    float curvatureAhead(float distance, float span) const;

    /** @return the nearest safe place (flat road, no gap or ramp nearby) at or before a segment */
    int safeSampleAtOrBefore(int segment) const;

    /** Signed distance (positive ahead) between two distances along the track, the short way round. */
    float delta(float from, float to) const;

    /** Bounds of the centreline, for the minimap. */
    void bounds(Vec2& outMin, Vec2& outMax) const;

private:
    std::string name_ = "Track";
    int laps_ = 3;
    float length_ = 0.0f;
    TrackTheme theme_;
    std::vector<TrackSample> samples_;
    std::vector<float> curvature_; // per sample, radians per metre

    bool finish(std::string& error);
};

/** Parses a colour written as #RRGGBB or 0xRRGGBB. */
bool parseColour(const char* text, uint32_t& out);

} // namespace hd
