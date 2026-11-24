// Ray-tracing scene built from the current frame's
// drawn polygons as one DXR acceleration structure (a bottom-level structure
// of camera-space triangles and a top-level one with a single instance),
// rebuilt every frame on SDL_GPU's own Direct3D 12 device and queue (published
// by patches/sdl3/0002-d3d12-native-handles.patch).
//
// Nothing reads the structure yet, so the image is unchanged. It is off unless
// M2_DXR_SCENE=1; at exit it prints the triangles per frame and the GPU build
// time measured with timestamp queries.
//
// Space: the rasterizer's camera space as GeoVertex keeps it, x and y already
// multiplied by the focal length (screen = centre + x / z), z = p[0]. A linear
// map of the true camera space, so ray hits stay exact; a direction such as
// the sun's must be scaled the same way (x by focus_x, y by focus_y).
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app {

class DxrScene {
public:
    DxrScene();
    ~DxrScene();
    DxrScene(const DxrScene &) = delete;
    DxrScene &operator=(const DxrScene &) = delete;

    // True when the device is Direct3D 12 with ray tracing tier 1.1 (inline
    // ray queries). Otherwise false, with error(); nothing else changes.
    bool init(SDL_GPUDevice *dev);
    bool ok() const;
    const std::string &error() const { return error_; }

    // What the shadow pass needs from the frame (GpuRenderer::ShadowFrame,
    // plus the sun): view_z (R32_FLOAT) and light (R8_UNORM) are the D3D12
    // resources of SDL textures in SDL's sampled-texture state, left so.
    struct Shade {
        void *view_z = nullptr, *light = nullptr;
        int width = 0, height = 0;
        bool large_supersampled = false;
        float to_screen[2] = {1, 1}, origin[2] = {0, 0};
        float sun[3] = {0, 1, 0}; // towards the sun, in the scaled camera space
        float strength = 0.6f;    // light left in shadow
        float bias = 0.002f;      // ray start, a fraction of the view z
        // sky light (ambient occlusion): the share of the light the sky gives (0 = off),
        // how far a sky ray looks for an occluder (camera units), the focal lengths
        float sky = 0.0f, sky_radius = 3.0f;
        float sun_radius = 0.0f; // the sun's disc, angular radius in radians (0 = hard edges)
        // Light shafts: sunlit air seen between shadows (a tunnel's openings),
        // added to the frame; 0 = off. shaft_dist: how far the eye ray is
        // marched, camera units.
        float shaft = 0.0f, shaft_dist = 60.0f;
        float shaft_phase = 0.0f; // fractional world-space step at the eye
        float focus[2] = {1, 1};
        // the player's car's own camera (its pixels have a negative view z)
        float origin2[2] = {0, 0}, focus2[2] = {1, 1};
        // cut-out triangles (9 floats each) and their texture (8 words each:
        // info.x, info.y, u v at each corner), tested against texture RAM
        const std::vector<float> *cutout_xyz = nullptr;
        const std::vector<uint32_t> *cutout_records = nullptr;
        const void *tex0 = nullptr, *tex1 = nullptr; // the two sheets, 0x80000 words each
        uint64_t tex_generation = 0;
    };

    // Records and executes the build of this frame's structure on the queue,
    // then, with `shade`, one ray per pixel towards the sun writing the light
    // texture. `xyz` holds 9 floats per triangle. True when the light texture
    // was written (with `shade`) or the structure built (without). Call after
    // submitting the SDL command buffer that drew view_z, before the one that
    // reads the light.
    bool build(const std::vector<float> &xyz, const Shade *shade = nullptr);

    // "daytona: DXR scene ..." summary (frames, triangles, GPU build time).
    void report() const;
    // Reports, waits for the queue, then releases everything. Must run before
    // SDL_DestroyGPUDevice, which unloads d3d12.dll.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

} // namespace app
