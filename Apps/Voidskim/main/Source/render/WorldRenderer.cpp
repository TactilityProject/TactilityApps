#include "WorldRenderer.h"

#include <cstring>

#include "Draw2D.h"

#include "Jet.hpp"

#include <algorithm>
#include <functional>

namespace hd {

using Renderer::Material;
using Renderer::Object;


// Jet works in integer world units: 16 per metre keeps a 2 km circuit well inside its range
constexpr float UNITS_PER_METRE = 16.0f;
constexpr int CHUNK_SAMPLES = 8; // track samples per mesh, the unit Jet culls by
constexpr float WALL_HEIGHT = 1.1f;
constexpr float WALL_THICKNESS = 0.5f;
constexpr float SLAB_DEPTH = 1.6f; // thickness of the track
constexpr float CLIFF_DEPTH = 26.0f; // how far the rock goes down on either side of a gap
constexpr int CLIFF_SAMPLES = 3; // road samples on either side of a gap that stand on rock
constexpr float HOVER_HEIGHT = 0.75f;
constexpr float FOV_DEGREES = 72.0f;

namespace {

struct P3 {
    float x, y, z;
    P3 operator-(const P3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    P3 operator+(const P3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    P3 operator*(float s) const { return { x * s, y * s, z * s }; }
};

P3 cross(const P3& a, const P3& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float dot3(const P3& a, const P3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

P3 at(const Vec2& p, float y) { return { p.x, y, p.z }; }

// Rounds to the nearest integer (std::lround's lroundf isn't available to apps)
int32_t roundToInt(float v) { return (int32_t)std::floor(v + 0.5f); }

Vector3 toJet(const P3& p) {
    return { roundToInt(p.x * UNITS_PER_METRE), roundToInt(p.y * UNITS_PER_METRE),
             roundToInt(p.z * UNITS_PER_METRE) };
}

/** Scales 0xRRGGBB by a brightness factor. */
uint32_t shadeRgb(uint32_t rgb, float factor) {
    auto channel = [&](int shift) {
        const int v = (int)((float)((rgb >> shift) & 0xFF) * factor);
        return (uint32_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

uint32_t mixRgb(uint32_t a, uint32_t b, float t) {
    auto channel = [&](int shift) {
        return (uint32_t)lerpf((float)((a >> shift) & 0xFF), (float)((b >> shift) & 0xFF), t);
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

} // namespace

/**
 * Adds flat-shaded faces to an Object. Jet's lighting is compiled out (it costs per vertex every
 * frame), so each face gets a fixed brightness from its normal and a fixed sun direction instead.
 */
class MeshBuilder {
public:
    MeshBuilder(Object* object, std::function<Material*(uint32_t)> material)
        : object(object), material(std::move(material)) {}

    /**
     * Adds a quad facing roughly along `facing`; the winding is chosen to match, since Jet culls
     * back faces.
     */
    void quad(const P3& a, const P3& b, const P3& c, const P3& d, uint32_t rgb, const P3& facing, bool shaded = true, Material* forced = nullptr) {
        P3 normal = cross(b - a, c - a);
        const bool flip = dot3(normal, facing) < 0.0f;
        if (flip) {
            normal = normal * -1.0f;
        }
        const uint16_t base = (uint16_t)object->vertices.size();
        addVertex(a);
        addVertex(b);
        addVertex(c);
        addVertex(d);
        Material* m = forced != nullptr ? forced : material(shaded ? shade(rgb, normal) : rgb);
        if (flip) {
            object->addFace(base, (uint16_t)(base + 3), (uint16_t)(base + 2), (uint16_t)(base + 1), m);
        } else {
            object->addFace(base, (uint16_t)(base + 1), (uint16_t)(base + 2), (uint16_t)(base + 3), m);
        }
    }

    void triangle(const P3& a, const P3& b, const P3& c, uint32_t rgb, const P3& facing) {
        P3 normal = cross(b - a, c - a);
        const bool flip = dot3(normal, facing) < 0.0f;
        if (flip) {
            normal = normal * -1.0f;
        }
        const uint16_t base = (uint16_t)object->vertices.size();
        addVertex(a);
        addVertex(b);
        addVertex(c);
        Material* m = material(shade(rgb, normal));
        if (flip) {
            object->addTriangle(base, (uint16_t)(base + 2), (uint16_t)(base + 1), m);
        } else {
            object->addTriangle(base, (uint16_t)(base + 1), (uint16_t)(base + 2), m);
        }
    }

    /** A box given its 8 corners: bottom 0-3 then top 4-7, each going around the same way. */
    /** @param back when set, the material of the -z (rear) face */
    void hexahedron(const P3 corner[8], uint32_t rgb, bool bottom = true, Material* back = nullptr) {
        P3 centre { 0, 0, 0 };
        for (int i = 0; i < 8; i++) centre = centre + corner[i] * 0.125f;
        auto face = [&](int a, int b, int c, int d, Material* forced = nullptr) {
            const P3 mid = (corner[a] + corner[b] + corner[c] + corner[d]) * 0.25f;
            quad(corner[a], corner[b], corner[c], corner[d], rgb, mid - centre, true, forced);
        };
        face(4, 5, 6, 7); // top
        if (bottom) face(0, 3, 2, 1);
        face(0, 1, 5, 4, back);
        face(1, 2, 6, 5);
        face(2, 3, 7, 6);
        face(3, 0, 4, 7);
    }

    /** An axis-aligned box from its centre and size. */
    void box(const P3& centre, const P3& size, uint32_t rgb, bool bottom = true, Material* back = nullptr) {
        const float hx = size.x * 0.5f, hy = size.y * 0.5f, hz = size.z * 0.5f;
        const P3 c[8] = {
            { centre.x - hx, centre.y - hy, centre.z - hz }, { centre.x + hx, centre.y - hy, centre.z - hz },
            { centre.x + hx, centre.y - hy, centre.z + hz }, { centre.x - hx, centre.y - hy, centre.z + hz },
            { centre.x - hx, centre.y + hy, centre.z - hz }, { centre.x + hx, centre.y + hy, centre.z - hz },
            { centre.x + hx, centre.y + hy, centre.z + hz }, { centre.x - hx, centre.y + hy, centre.z + hz },
        };
        hexahedron(c, rgb, bottom, back);
    }

    static uint32_t shade(uint32_t rgb, const P3& normal) {
        // Sun high, from the front left. A face pointing straight up keeps its exact colour.
        const P3 sun { -0.35f, 0.85f, -0.4f };
        const float len = std::sqrt(dot3(normal, normal));
        const float sunLen = std::sqrt(dot3(sun, sun));
        if (len < 1e-6f) return rgb;
        const float lambert = std::max(0.0f, dot3(normal, sun) / (len * sunLen));
        const float up = sun.y / sunLen;
        return shadeRgb(rgb, std::min(1.1f, 0.45f + 0.55f * lambert / up));
    }

private:
    Object* object;
    std::function<Material*(uint32_t)> material;

    void addVertex(const P3& p) {
        Object::Vertex v;
        v.position = toJet(p);
        object->addVertex(v);
    }
};

WorldRenderer::WorldRenderer() = default;

WorldRenderer::~WorldRenderer() {
    clear();
}

bool WorldRenderer::init(uint16_t* buffer, int w, int h) {
    output = buffer;
    outputWidth = w;
    outputHeight = h;
    applyResolution();
    return true;
}

void WorldRenderer::setResolutionPercent(int percent) {
    percent = std::max(25, std::min(100, percent));
    if (percent == resolutionPercent) return;
    resolutionPercent = percent;
    clear(); // the scene is sized for the old resolution
    applyResolution();
}

void WorldRenderer::applyResolution() {
    if (resolutionPercent >= 100) {
        width = outputWidth;
        height = outputHeight;
        framebuffer = output;
        lowResolution.clear();
        lowResolution.shrink_to_fit();
        columnSource.clear();
    } else {
        width = std::max(16, outputWidth * resolutionPercent / 100);
        height = std::max(16, outputHeight * resolutionPercent / 100);
        lowResolution.assign((size_t)width * height, 0);
        framebuffer = lowResolution.data();
        columnSource.resize(outputWidth);
        for (int x = 0; x < outputWidth; x++) {
            columnSource[x] = (uint16_t)std::min(width - 1, (x * width + width / 2) / outputWidth);
        }
    }
    skyRows.assign(height, 0);
}

void WorldRenderer::scaleUp() {
    // Nearest neighbour: each output row copies a low resolution row, or repeats the row above it
    int previousSource = -1;
    for (int y = 0; y < outputHeight; y++) {
        const int sourceRow = std::min(height - 1, (y * height + height / 2) / outputHeight);
        uint16_t* destination = output + (size_t)y * outputWidth;
        if (sourceRow == previousSource) {
            memcpy(destination, destination - outputWidth, (size_t)outputWidth * sizeof(uint16_t));
            continue;
        }
        const uint16_t* source = framebuffer + (size_t)sourceRow * width;
        const uint16_t* map = columnSource.data();
        for (int x = 0; x < outputWidth; x++) {
            destination[x] = source[map[x]];
        }
        previousSource = sourceRow;
    }
}

Material* WorldRenderer::material(uint32_t rgb) {
    auto found = materialsByColour.find(rgb);
    if (found != materialsByColour.end()) {
        return found->second;
    }
    Material* m = ownMaterial(rgb);
    materialsByColour[rgb] = m;
    return m;
}

Material* WorldRenderer::ownMaterial(uint32_t rgb) {
    materials.push_back(std::make_unique<Material>(rgb565(rgb)));
    return materials.back().get();
}

void WorldRenderer::clear() {
    crafts.clear();
    engines.clear();
    rolls.clear();
    scene.reset();
    camera.reset();
    objects.clear();
    materials.clear();
    materialsByColour.clear();
    cameraValid = false;
}

void WorldRenderer::build(const RaceSim& race) {
    clear();
    scene = std::make_unique<Renderer::Scene>(framebuffer, nullptr, width, height);
    scene->setClearBuffer(true);
    camera = std::make_unique<Renderer::Camera>();
    camera->setFOV(FOV_DEGREES, width);
    camera->nearPlane = 6;
    camera->farPlane = (int32_t)(170.0f * UNITS_PER_METRE);
    scene->setCamera(camera.get());

    const TrackTheme& theme = race.track().theme();
    skyTop = theme.skyTop;
    skyHorizon = theme.skyHorizon;
    below = theme.below;
    scene->setBackcolor(rgb565(below));
    scene->backgroundGradientColors = skyRows.data();

    buildTrack(race.track());

    for (const auto& racer : race.racers()) {
        Material* engine = nullptr;
        Object* craft = buildCraft(race.vehicle(racer), engine);
        crafts.push_back(craft);
        engines.push_back(engine);
        rolls.push_back(0.0f);
    }
}

void WorldRenderer::buildTrack(const Track& track) {
    const TrackTheme& theme = track.theme();
    auto materialFor = [this](uint32_t rgb) { return material(rgb); };
    const int count = track.sampleCount();
    const P3 up { 0, 1, 0 };
    const P3 down { 0, -1, 0 };
    Random random(0xC0FFEEu ^ (uint32_t)count);

    for (int chunkStart = 0; chunkStart < count; chunkStart += CHUNK_SAMPLES) {
        auto object = std::make_unique<Object>();
        MeshBuilder mesh(object.get(), materialFor);

        for (int k = chunkStart; k < std::min(count, chunkStart + CHUNK_SAMPLES); k++) {
            const TrackSample& a = track.sample(k);
            const TrackSample& b = track.sample(k + 1);
            if (a.gap) {
                continue;
            }
            // The segment after a ramp top is at ground level: the ramp ends in a sheer drop
            const float ya = a.rampTop ? b.height : a.height;
            const float yb = b.height;
            const P3 forward { a.direction.x, 0, a.direction.z };
            const P3 rightA { a.right.x, 0, a.right.z };
            const P3 leftA = rightA * -1.0f;
            // Either side of a gap the road stands on deep rock, so the gap reads as an abyss
            bool nearGap = false;
            for (int n = -CLIFF_SAMPLES; n <= CLIFF_SAMPLES + 1 && !nearGap; n++) {
                nearGap = track.sample(k + n).gap;
            }
            const float slabDepth = nearGap ? CLIFF_DEPTH : SLAB_DEPTH;

            // Road: alternating bands give a sense of speed; ramps get warning colours
            const bool band = ((k / 2) % 2) == 0;
            uint32_t roadColour = band ? theme.roadA : theme.roadB;
            if (a.ramp && !a.rampTop) {
                roadColour = band ? theme.ramp : shadeRgb(theme.ramp, 0.55f);
            }
            const bool fullWidth = a.wallLeft && a.wallRight;
            const float innerA = a.halfWidth - (fullWidth ? 0.0f : CURB_WIDTH);
            const float innerB = b.halfWidth - (fullWidth ? 0.0f : CURB_WIDTH);
            const float roadLeftA = a.wallLeft ? a.halfWidth : innerA;
            const float roadLeftB = a.wallLeft ? b.halfWidth : innerB;
            const float roadRightA = a.wallRight ? a.halfWidth : innerA;
            const float roadRightB = a.wallRight ? b.halfWidth : innerB;

            if (k == 0) {
                // Start line: a band of checks across the road
                const int checks = 8;
                for (int c = 0; c < checks; c++) {
                    for (int row = 0; row < 2; row++) {
                        const float u0 = (float)c / checks;
                        const float u1 = (float)(c + 1) / checks;
                        const float v0 = row * 0.5f;
                        const float v1 = (row + 1) * 0.5f;
                        auto point = [&](float u, float v) {
                            const Vec2 l0 = a.position - a.right * roadLeftA;
                            const Vec2 r0 = a.position + a.right * roadRightA;
                            const Vec2 l1 = b.position - b.right * roadLeftB;
                            const Vec2 r1 = b.position + b.right * roadRightB;
                            const Vec2 p0 = l0 + (r0 - l0) * u;
                            const Vec2 p1 = l1 + (r1 - l1) * u;
                            return at(p0 + (p1 - p0) * v, ya);
                        };
                        const uint32_t colour = ((c + row) % 2 == 0) ? 0xF0F0F0 : 0x151515;
                        mesh.quad(point(u0, v0), point(u0, v1), point(u1, v1), point(u1, v0), colour, up, false);
                    }
                }
            } else {
                mesh.quad(at(a.position - a.right * roadLeftA, ya), at(b.position - b.right * roadLeftB, yb),
                          at(b.position + b.right * roadRightB, yb), at(a.position + a.right * roadRightA, ya),
                          roadColour, up, false);
            }

            // Each edge: a barrier, or striped curbs where you can fall off
            for (int side = -1; side <= 1; side += 2) {
                const bool wall = side < 0 ? a.wallLeft : a.wallRight;
                const P3 outward = (side < 0 ? leftA : rightA);
                const Vec2 edgeA = a.position + a.right * (a.halfWidth * side);
                const Vec2 edgeB = b.position + b.right * (b.halfWidth * side);
                if (wall && a.tunnel) {
                    // Tunnel wall: up to the roof
                    const Vec2 outerA = edgeA + a.right * (WALL_THICKNESS * side);
                    const Vec2 outerB = edgeB + b.right * (WALL_THICKNESS * side);
                    const uint32_t wallColour = (k % 4 == 0) ? shadeRgb(theme.wall, 0.45f) : shadeRgb(theme.wall, 0.6f);
                    mesh.quad(at(edgeA, ya), at(edgeB, yb), at(edgeB, yb + TUNNEL_CLEARANCE), at(edgeA, ya + TUNNEL_CLEARANCE), wallColour, outward * -1.0f);
                    mesh.quad(at(outerA, ya - SLAB_DEPTH), at(outerB, yb - SLAB_DEPTH), at(outerB, yb + TUNNEL_CLEARANCE + 0.6f), at(outerA, ya + TUNNEL_CLEARANCE + 0.6f), theme.structure, outward);
                } else if (wall) {
                    const Vec2 outerA = edgeA + a.right * (WALL_THICKNESS * side);
                    const Vec2 outerB = edgeB + b.right * (WALL_THICKNESS * side);
                    const uint32_t wallColour = (k % 4 == 0) ? shadeRgb(theme.wall, 0.7f) : theme.wall;
                    // Inner face, top, outer face
                    mesh.quad(at(edgeA, ya), at(edgeB, yb), at(edgeB, yb + WALL_HEIGHT), at(edgeA, ya + WALL_HEIGHT), wallColour, outward * -1.0f);
                    mesh.quad(at(edgeA, ya + WALL_HEIGHT), at(edgeB, yb + WALL_HEIGHT), at(outerB, yb + WALL_HEIGHT), at(outerA, ya + WALL_HEIGHT), wallColour, up);
                    mesh.quad(at(outerA, ya - slabDepth), at(outerB, yb - slabDepth), at(outerB, yb + WALL_HEIGHT), at(outerA, ya + WALL_HEIGHT), theme.structure, outward);
                } else {
                    const Vec2 innerEdgeA = a.position + a.right * (innerA * side);
                    const Vec2 innerEdgeB = b.position + b.right * (innerB * side);
                    const uint32_t curb = (k % 2 == 0) ? theme.edgeA : theme.edgeB;
                    mesh.quad(at(innerEdgeA, ya), at(innerEdgeB, yb), at(edgeB, yb), at(edgeA, ya), curb, up, false);
                    // The side of the slab, seen when looking over the edge
                    mesh.quad(at(edgeA, ya - slabDepth), at(edgeB, yb - slabDepth), at(edgeB, yb), at(edgeA, ya), theme.structure, outward);
                }
                // Ramps: the triangle under the raised road
                if (a.ramp && !a.rampTop && (yb > b.base + 0.01f || ya > a.base + 0.01f)) {
                    mesh.quad(at(edgeA, a.base), at(edgeB, b.base), at(edgeB, yb), at(edgeA, ya), shadeRgb(theme.ramp, 0.5f), outward);
                }
            }

            // Ramp top: the face of the drop
            if (a.rampTop && a.height > a.base + 0.01f) {
                const Vec2 l = a.position - a.right * a.halfWidth;
                const Vec2 r = a.position + a.right * a.halfWidth;
                mesh.quad(at(l, a.base), at(r, a.base), at(r, a.height), at(l, a.height), shadeRgb(theme.ramp, 0.4f), forward);
            }

            // Gap edges: sheer rock faces down into the abyss, with a hazard stripe along the lip. The
            // far face is the one you see coming: the jump has to clear it.
            const bool gapBefore = track.sample(k - 1).gap;
            const bool gapAfter = b.gap;
            if (gapBefore || gapAfter) {
                const TrackSample& edge = gapBefore ? a : b;
                const float top = gapBefore ? ya : yb;
                const P3 facing = gapBefore ? forward * -1.0f : forward;
                const Vec2 l = edge.position - edge.right * edge.halfWidth;
                const Vec2 r = edge.position + edge.right * edge.halfWidth;
                // Hazard stripe
                const int stripes = 6;
                for (int i = 0; i < stripes; i++) {
                    const Vec2 p0 = l + (r - l) * ((float)i / stripes);
                    const Vec2 p1 = l + (r - l) * ((float)(i + 1) / stripes);
                    mesh.quad(at(p0, top - 1.4f), at(p1, top - 1.4f), at(p1, top), at(p0, top), (i % 2 == 0) ? theme.edgeA : theme.edgeB, facing, false);
                }
                // Rock, in bands that get darker with depth
                const int bands = 5;
                const float bandHeight = (CLIFF_DEPTH - 1.4f) / bands;
                for (int i = 0; i < bands; i++) {
                    const float y0 = top - 1.4f - bandHeight * i;
                    const float y1 = y0 - bandHeight;
                    const uint32_t colour = shadeRgb(theme.structure, (i % 2 == 0 ? 0.85f : 0.7f) * (1.0f - 0.12f * i));
                    mesh.quad(at(l, y1), at(r, y1), at(r, y0), at(l, y0), colour, facing, false);
                }
            }

            // Tunnel roof: its underside seen from inside, its top from above
            if (a.tunnel) {
                const float wallOut = WALL_THICKNESS;
                const Vec2 l0 = a.position - a.right * (a.halfWidth + wallOut);
                const Vec2 r0 = a.position + a.right * (a.halfWidth + wallOut);
                const Vec2 l1 = b.position - b.right * (b.halfWidth + wallOut);
                const Vec2 r1 = b.position + b.right * (b.halfWidth + wallOut);
                const float roofA = ya + TUNNEL_CLEARANCE;
                const float roofB = yb + TUNNEL_CLEARANCE;
                mesh.quad(at(l0, roofA), at(l1, roofB), at(r1, roofB), at(r0, roofA), shadeRgb(theme.structure, (k % 2 == 0) ? 0.35f : 0.45f), down, false);
                mesh.quad(at(l0, roofA + 0.6f), at(l1, roofB + 0.6f), at(r1, roofB + 0.6f), at(r0, roofA + 0.6f), theme.structure, up);
            }

            // Underside, seen when falling
            mesh.quad(at(a.position - a.right * a.halfWidth, ya - slabDepth), at(b.position - b.right * b.halfWidth, yb - slabDepth),
                      at(b.position + b.right * b.halfWidth, yb - slabDepth), at(a.position + a.right * a.halfWidth, ya - slabDepth),
                      shadeRgb(theme.structure, 0.6f), down, false);

            // Pillars holding the track up, except where they'd stand on the road below
            if (k % 6 == 3 && !a.overpass) {
                mesh.box(at(a.position, ya - SLAB_DEPTH - 30.0f), { 2.2f, 60.0f, 2.2f }, theme.structure, false);
            }

            // Towers beside the track
            if (k % 14 == 7) {
                const float side = random.unit() < 0.5f ? -1.0f : 1.0f;
                const float distance = a.halfWidth + random.range(14.0f, 45.0f);
                const float towerHeight = random.range(12.0f, 45.0f);
                const float size = random.range(5.0f, 10.0f);
                const Vec2 base = a.position + a.right * (distance * side);
                const uint32_t colour = mixRgb(theme.scenery, theme.structure, random.unit() * 0.5f);
                mesh.box(at(base, towerHeight * 0.5f - 40.0f), { size, towerHeight + 40.0f, size }, colour, false);
                // A light on top
                mesh.box(at(base, towerHeight + 0.6f), { size * 0.4f, 1.2f, size * 0.4f }, theme.skyHorizon, false);
            }
        }

        if (!object->vertices.empty()) {
            object->calculateBoundingBox();
            scene->addObject(object.get());
            objects.push_back(std::move(object));
        }
    }

    // Start gantry: two posts and a beam over the line
    {
        auto object = std::make_unique<Object>();
        MeshBuilder mesh(object.get(), materialFor);
        const TrackSample& s = track.sample(0);
        const float postHeight = 7.0f;
        for (int side = -1; side <= 1; side += 2) {
            const Vec2 post = s.position + s.right * ((s.halfWidth + 1.2f) * side);
            mesh.box(at(post, postHeight * 0.5f - 1.0f), { 1.2f, postHeight + 2.0f, 1.2f }, theme.structure);
        }
        const Vec2 beam = s.position;
        const float span = s.halfWidth * 2.0f + 3.6f;
        // The beam runs across the track: build it from the track's right direction
        const P3 r { s.right.x, 0, s.right.z };
        const P3 f { s.direction.x, 0, s.direction.z };
        const P3 c = at(beam, postHeight);
        const P3 hx = r * (span * 0.5f);
        const P3 hz = f * 0.6f;
        const P3 hy { 0, 0.9f, 0 };
        const P3 corners[8] = {
            c - hx - hz - hy, c + hx - hz - hy, c + hx + hz - hy, c - hx + hz - hy,
            c - hx - hz + hy, c + hx - hz + hy, c + hx + hz + hy, c - hx + hz + hy,
        };
        mesh.hexahedron(corners, theme.edgeB);
        object->calculateBoundingBox();
        scene->addObject(object.get());
        objects.push_back(std::move(object));
    }
}

Object* WorldRenderer::buildCraft(const VehicleSpec& spec, Material*& engine) {
    auto object = std::make_unique<Object>();
    auto materialFor = [this](uint32_t rgb) { return material(rgb); };
    MeshBuilder mesh(object.get(), materialFor);
    const uint32_t body = spec.colour;
    const uint32_t trim = spec.accent;
    const uint32_t canopy = 0x1B263B;

    // Engine glow: the rear faces of the engines, in their own material that's recoloured while
    // boosting. Part of the engine boxes themselves rather than a separate quad behind them, which
    // the painter's sort would sometimes draw in front of the hull and sometimes behind it.
    engine = ownMaterial(0xFF8C1A);

    // Model space: +z is the nose, +y up, origin at the centre of the craft's footprint
    switch (spec.shape) {
        case CraftShape::Dart: {
            // Long tapered body
            const P3 hull[8] = {
                { -0.7f, 0.0f, -1.8f }, { 0.7f, 0.0f, -1.8f }, { 0.15f, 0.1f, 2.4f }, { -0.15f, 0.1f, 2.4f },
                { -0.6f, 0.7f, -1.6f }, { 0.6f, 0.7f, -1.6f }, { 0.1f, 0.25f, 2.4f }, { -0.1f, 0.25f, 2.4f },
            };
            mesh.hexahedron(hull, body);
            // Engines on outriggers
            for (int side = -1; side <= 1; side += 2) {
                mesh.box({ side * 1.35f, 0.35f, -1.0f }, { 0.6f, 0.6f, 2.2f }, trim, true, engine);
                mesh.box({ side * 0.9f, 0.3f, -0.8f }, { 0.5f, 0.15f, 1.0f }, body);
            }
            mesh.box({ 0.0f, 0.85f, -0.3f }, { 0.5f, 0.3f, 1.0f }, canopy);
            mesh.box({ 0.0f, 1.1f, -1.5f }, { 0.12f, 0.8f, 0.6f }, trim); // fin
            break;
        }
        case CraftShape::Wing: {
            // Flat delta
            const P3 wing[8] = {
                { -2.0f, 0.1f, -1.6f }, { 2.0f, 0.1f, -1.6f }, { 0.25f, 0.15f, 2.0f }, { -0.25f, 0.15f, 2.0f },
                { -1.9f, 0.35f, -1.5f }, { 1.9f, 0.35f, -1.5f }, { 0.2f, 0.3f, 2.0f }, { -0.2f, 0.3f, 2.0f },
            };
            mesh.hexahedron(wing, body);
            mesh.box({ 0.0f, 0.55f, 0.2f }, { 0.7f, 0.4f, 1.4f }, canopy);
            for (int side = -1; side <= 1; side += 2) {
                mesh.box({ side * 1.7f, 0.55f, -1.3f }, { 0.15f, 0.6f, 0.6f }, trim); // wingtip fins
                mesh.box({ side * 0.6f, 0.35f, -1.4f }, { 0.5f, 0.4f, 0.6f }, trim, true, engine);
            }
            break;
        }
        case CraftShape::Hauler: {
            // Two heavy pods joined by a cockpit beam
            for (int side = -1; side <= 1; side += 2) {
                const P3 pod[8] = {
                    { side * 1.0f - 0.55f, 0.0f, -1.9f }, { side * 1.0f + 0.55f, 0.0f, -1.9f },
                    { side * 1.0f + 0.4f, 0.05f, 1.9f }, { side * 1.0f - 0.4f, 0.05f, 1.9f },
                    { side * 1.0f - 0.55f, 0.9f, -1.9f }, { side * 1.0f + 0.55f, 0.9f, -1.9f },
                    { side * 1.0f + 0.4f, 0.6f, 1.9f }, { side * 1.0f - 0.4f, 0.6f, 1.9f },
                };
                mesh.hexahedron(pod, body, true, engine);
                mesh.box({ side * 1.0f, 0.95f, -0.6f }, { 0.7f, 0.15f, 1.6f }, trim);
            }
            mesh.box({ 0.0f, 0.45f, 0.4f }, { 1.4f, 0.4f, 1.8f }, trim);
            mesh.box({ 0.0f, 0.8f, 0.7f }, { 0.7f, 0.35f, 0.9f }, canopy);
            break;
        }
    }

    object->calculateBoundingBox();
    object->preciseDepthSort = true; // small overlapping parts
    Object* raw = object.get();
    scene->addObject(raw);
    objects.push_back(std::move(object));
    return raw;
}

void WorldRenderer::setCamera(float x, float y, float z, float targetX, float targetY, float targetZ) {
    const float dx = targetX - x;
    const float dy = targetY - y;
    const float dz = targetZ - z;
    const float horizontal = std::sqrt(dx * dx + dz * dz);
    const float yaw = std::atan2(dx, dz);
    const float pitch = std::atan2(-dy, horizontal); // Jet: positive pitch looks down
    camera->setPosition((int32_t)(x * UNITS_PER_METRE), (int32_t)(y * UNITS_PER_METRE), (int32_t)(z * UNITS_PER_METRE));
    float wrappedYaw = yaw < 0.0f ? yaw + 2.0f * PI : yaw;
    float wrappedPitch = pitch < 0.0f ? pitch + 2.0f * PI : pitch;
    camera->rotation.x = wrappedPitch;
    camera->rotation.y = wrappedYaw;
    camera->rotation.z = 0.0f;
    updateSky(pitch);
}

void WorldRenderer::updateSky(float pitch) {
    // Where the horizon is on screen for this camera pitch
    const float focal = (float)(width / 2) / std::tan(FOV_DEGREES * 0.5f * DEG_TO_RAD);
    const float horizon = (float)height * 0.5f - std::tan(pitch) * focal;
    const uint16_t belowColour = rgb565(below);
    for (int y = 0; y < height; y++) {
        uint32_t colour;
        if (y < horizon) {
            const float t = clampf((float)y / std::max(1.0f, horizon), 0.0f, 1.0f);
            colour = mixRgb(skyTop, skyHorizon, t * t);
        } else {
            // A thin haze band, then the void
            const float t = clampf((y - horizon) / ((float)height * 0.12f), 0.0f, 1.0f);
            colour = mixRgb(skyHorizon, below, t);
        }
        skyRows[y] = y < horizon || colour != below ? rgb565(colour) : belowColour;
    }
}

void WorldRenderer::update(const RaceSim& race, int focus, float time, float frameTime) {
    if (!scene) {
        return;
    }
    for (const auto& racer : race.racers()) {
        Object* craft = crafts[racer.id];
        craft->enabled = racer.mode != RacerMode::Respawning;
        // Blink while ghosting after a respawn
        if (racer.ghostTime > 0.0f && ((int)(time * 10.0f) % 2) == 0) {
            craft->enabled = false;
        }
        const float bob = racer.airborne ? 0.0f : std::sin(time * 5.0f + (float)racer.id) * 0.08f;
        craft->setPosition(toJet({ racer.position.x, racer.height + HOVER_HEIGHT + bob, racer.position.z }));
        // Bank into turns (eased, so it doesn't snap with the steering), pitch with the climb
        const float bankSmoothing = frameTime > 0.0f ? 1.0f - std::exp(-frameTime * 8.0f) : 1.0f;
        rolls[racer.id] += (-racer.input.steer * 14.0f - rolls[racer.id]) * bankSmoothing;
        const float roll = rolls[racer.id];
        const float pitch = racer.airborne ? clampf(-racer.verticalSpeed * 1.2f, -20.0f, 20.0f) : 0.0f;
        auto wrapDegrees = [](float d) { int v = (int)roundToInt(d) % 360; return v < 0 ? v + 360 : v; };
        craft->setRotation(wrapDegrees(pitch), wrapDegrees(racer.heading * RAD_TO_DEG), wrapDegrees(roll));
        // Engine: flickering orange, hot white-blue when boosting
        const float flicker = 0.85f + 0.15f * std::sin(time * 40.0f + (float)racer.id * 3.0f);
        const uint32_t glow = racer.boosting ? 0x9FE7FF : shadeRgb(0xFF8C1A, flicker);
        engines[racer.id]->color = rgb565(glow);
    }

    const Racer& racer = race.racer(focus);
    const Vec2 forward = headingToDirection(racer.heading);
    // Camera direction follows the craft's heading, smoothed so steering isn't twitchy
    const float targetYaw = racer.heading;
    const float smoothing = 1.0f - std::exp(-frameTime * 6.0f);
    if (!cameraValid) {
        cameraYaw = targetYaw;
    }
    cameraYaw = wrapAngle(cameraYaw + wrapAngle(targetYaw - cameraYaw) * smoothing);
    const Vec2 cameraForward = headingToDirection(cameraYaw);

    if (racer.mode == RacerMode::Driving || !cameraValid) {
        const float baseHeight = std::max(racer.height, 0.0f);
        const float wantX = racer.position.x - cameraForward.x * 8.5f;
        const float wantZ = racer.position.z - cameraForward.z * 8.5f;
        const float wantY = baseHeight + 3.4f;
        const float follow = cameraValid ? 1.0f - std::exp(-frameTime * 10.0f) : 1.0f;
        cameraX = lerpf(cameraX, wantX, follow);
        cameraY = lerpf(cameraY, wantY, follow);
        cameraZ = lerpf(cameraZ, wantZ, follow);
        cameraValid = true;
    }
    // While falling the camera stays put and watches the craft drop
    setCamera(cameraX, cameraY, cameraZ,
              racer.position.x + forward.x * 6.0f, racer.height + 1.2f, racer.position.z + forward.z * 6.0f);
}

void WorldRenderer::updateShowcase(const RaceSim& race, float time) {
    if (!scene) {
        return;
    }
    update(race, 0, time, 0.0f);
    // Circle around the grid
    Vec2 centre;
    Vec2 direction;
    race.track().pointAt(race.track().length() - 12.0f, centre, direction);
    const float angle = time * 0.2f;
    const float radius = 34.0f;
    setCamera(centre.x + std::sin(angle) * radius, 12.0f, centre.z + std::cos(angle) * radius, centre.x, 0.0f, centre.z);
    cameraValid = false;
}

void WorldRenderer::render() {
    if (scene) {
        scene->render();
        if (framebuffer != output) {
            scaleUp();
        }
    }
}

} // namespace hd
