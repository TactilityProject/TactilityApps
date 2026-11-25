#pragma once

#include "../core/Race.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace Renderer {
class Scene;
class Camera;
class Object;
class Material;
}

namespace hd {

/**
 * Draws the 3D world with Jet: the track, scenery and crafts, seen from a chase camera.
 * Knows nothing about input or the HUD; reads the RaceSim it's given.
 */
class WorldRenderer {
public:
    WorldRenderer();
    ~WorldRenderer();

    /** Renders into an RGB565 buffer of width x height pixels. */
    bool init(uint16_t* framebuffer, int width, int height);

    /**
     * The 3D resolution, as a percentage of the buffer's width and height (25..100). Below 100 the
     * world renders into a smaller buffer of its own, which render() scales up into the frame, so
     * whatever is drawn on top afterwards (HUD, menus) keeps the full resolution.
     * Takes effect at the next build().
     */
    void setResolutionPercent(int percent);

    /** Builds the meshes for a track and its racers. Replaces whatever was there. */
    void build(const RaceSim& race);

    /** Releases the meshes (e.g. when going back to the menu). */
    void clear();

    /**
     * Positions the crafts and camera for a frame.
     * @param focus the racer the camera follows
     * @param time seconds, for animation (hover bob, engine flicker)
     * @param frameTime seconds since the last frame, for camera smoothing
     */
    void update(const RaceSim& race, int focus, float time, float frameTime);

    /** Slow fly-around of the start, for menus. */
    void updateShowcase(const RaceSim& race, float time);

    void render();

private:
    std::unique_ptr<Renderer::Scene> scene;
    std::unique_ptr<Renderer::Camera> camera;
    std::vector<std::unique_ptr<Renderer::Object>> objects;
    std::vector<std::unique_ptr<Renderer::Material>> materials;
    std::unordered_map<uint32_t, Renderer::Material*> materialsByColour;
    std::vector<Renderer::Object*> crafts; // per racer
    std::vector<Renderer::Material*> engines; // per racer, recoloured when boosting
    std::vector<float> rolls; // per racer: the smoothed bank angle, in degrees
    std::vector<uint16_t> skyRows;
    // The frame (from init) and where the 3D is rendered: the frame itself, or a smaller buffer
    uint16_t* output = nullptr;
    int outputWidth = 0;
    int outputHeight = 0;
    int resolutionPercent = 100;
    std::vector<uint16_t> lowResolution;
    std::vector<uint16_t> columnSource; // per output column: the low resolution column it shows
    uint16_t* framebuffer = nullptr;
    int width = 0; // of the 3D rendering
    int height = 0;

    void applyResolution();
    void scaleUp();
    uint32_t skyTop = 0, skyHorizon = 0, below = 0;

    // Chase camera state, in metres
    float cameraX = 0, cameraY = 0, cameraZ = 0;
    float cameraYaw = 0;
    bool cameraValid = false;

    Renderer::Material* material(uint32_t rgb);
    Renderer::Material* ownMaterial(uint32_t rgb);
    void buildTrack(const Track& track);
    Renderer::Object* buildCraft(const VehicleSpec& spec, Renderer::Material*& engine);
    void setCamera(float x, float y, float z, float targetX, float targetY, float targetZ);
    void updateSky(float pitch);
};

} // namespace hd
