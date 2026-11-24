// Hardware renderer (SDL_GPU): draws a frame on the GPU instead of the CPU
// rasterizer (src/runtime/raster.cpp, the exact reference) and tilemap
// drawing (Video::draw): the back tilemap layers, the Model 2 3D layer, the
// front tilemap layers (Video's external-3D desktop mode).
//
// The tilemap layers are composed per pixel (m2.hlsl ps_tiles_back,
// ps_tiles_front) from the pixmaps the CPU decodes, uploaded by the rows of
// tiles that changed, and the frame's tile registers and pens. With the HUD
// moved to the widescreen edges the front layers come from the CPU instead.
//
// Polygons are projected exactly as model2_3d_project, drawn as a fan from
// their first vertex in the rasterizer's order (window, then z, newest first)
// and clipped to their window; the depth test stands in for the rasterizer's
// "first polygon to fill a pixel wins". Solid polygons take their palette
// colour from the CPU; textured ones are shaded per pixel by a port of the
// rasterizer's draw_tex_span (m2.hlsl) reading texture RAM, the luma RAM and
// the colour translation from storage buffers.
#pragma once

#include "app/gpu/opponent_bodies.h"
#include "runtime/video.h"

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <vector>

namespace app {

class GpuRenderer {
public:
    // Creates the pipelines for drawing into textures of `format`. False (with
    // error()) if the GPU or its shader format is not usable.
    bool init(SDL_GPUDevice *dev, SDL_GPUTextureFormat format);
    void shutdown();
    bool ok() const { return poly_pipe_ != nullptr; }
    const std::string &error() const { return error_; }

    // Draws the frame into `target` (at least w x h times `scale`, created
    // with COLOR_TARGET usage): the back tilemap layers, the 3D polygons, the
    // front tilemap layers, all from `video` in external-3D mode. scale > 1
    // (the internal resolution enhancement): the 3D drawn at w x h times
    // scale, textures a mip level finer per doubling, the tilemaps' pixels
    // repeated.
    bool render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h,
                const rt::Video &video, int scale = 1, int pixel_w = 0, int pixel_h = 0,
                bool sharp_textures = false, const std::vector<rt::GeoPoly> *pose_polys = nullptr);

    // Ray tracing (app/gpu/dxr_scene.h): when set, each render fills `out`
    // with the drawn polygons as camera-space triangles (9 floats each: x, y
    // already times the focal length, z), in the fan order the GPU draws.
    void collect_triangles(std::vector<float> *out) { triangles_ = out; }
    // Polygons that only cast shadows (rt::Geo::shadow_casters: behind the
    // camera, never drawn), added to the triangles of the next render.
    void set_shadow_casters(const std::vector<rt::GeoPoly> *casters) { casters_ = casters; }

    // Ray-traced sun shadows (app/gpu/dxr.hlsl). Once enabled, each render
    // also writes the view z of the polygon that won each pixel (0 where a
    // front layer covers the 3D) into a texture of the target's size; the
    // DXR pass turns it into a light texture, and apply_shadow multiplies
    // the target by it. Never enabled, nothing of this exists or runs.
    bool enable_shadows();
    struct ShadowFrame {
        void *view_z = nullptr, *light = nullptr; // ID3D12Resource of each
        int width = 0, height = 0;                // the target, in pixels
        float to_screen[2] = {1, 1};              // screen units per pixel
        float origin[2] = {0, 0};                 // screen position of the camera axis
        float focus[2] = {1, 1};                  // the polygons' focal lengths
        // the player's car's own camera (the game draws it with another centre
        // and focal length); its pixels carry a negative view z
        float origin2[2] = {0, 0}, focus2[2] = {1, 1};
        const std::vector<float> *cutout_xyz = nullptr;        // cut-out triangles (9 floats each)
        const std::vector<uint32_t> *cutout_records = nullptr; // their texture: info.x, info.y, u0 v0 u1 v1 u2 v2
        const void *tex0 = nullptr, *tex1 = nullptr;          // texture RAM sheets (0x80000 words each)
        uint64_t tex_generation = 0;
    };
    // The last render's, or nullptr when it drew no shadow inputs.
    const ShadowFrame *shadow_frame() const { return shadow_ready_ ? &shadow_frame_ : nullptr; }
    // diagnostic: the frame's cameras (origin x y, focus x y, window: polygons)
    const std::vector<std::pair<std::array<float, 5>, uint32_t>> &shadow_cameras() const { return cameras_; }
    uint32_t shadow_main_camera() const { return main_camera_; }
    bool apply_shadow(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target);

private:
    std::vector<float> *triangles_ = nullptr;
    const std::vector<rt::GeoPoly> *casters_ = nullptr;
    bool shadows_ = false, shadow_ready_ = false;
    SDL_GPUTextureFormat frame_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipeline *z_pipe_ = nullptr, *z_front_pipe_ = nullptr, *z_quad_pipe_ = nullptr,
                            *shade_pipe_ = nullptr, *car_z_pipe_ = nullptr;
    SDL_GPUTexture *view_z_ = nullptr, *light_ = nullptr;
    ShadowFrame shadow_frame_;
    int frame_w_ = 0, frame_h_ = 0;
    std::vector<std::pair<std::array<float, 5>, uint32_t>> cameras_; // origin x, y, focus x, y, window: polygons
    struct ShadowPoly { uint32_t first, count, batch, camera, tri_first, tri_count; bool cutout; }; // per drawn polygon
    struct ZDraw { uint32_t first, count, batch; };                                    // view z pass runs
    std::vector<ShadowPoly> shadow_polys_;
    std::vector<float> cutout_xyz_;        // cut-out triangles, 9 floats each
    std::vector<uint32_t> cutout_records_; // 8 words each: texture info.x, info.y, then u, v of each corner
    std::vector<ZDraw> z_draws_, car_z_draws_; // the main camera's, the player's car's
    uint32_t main_camera_ = ~0u, car_camera_ = ~0u;
    bool ensure_shadow_targets(int pixel_w, int pixel_h);
    struct PolyVertex {
        float x, y, depth;
        float ooz, uoz, voz; // render_one's p[0..2]: 1/z, u/z, v/z
        float r, g, b, a;
        uint32_t info[4], more[2]; // texture state (m2.hlsl PolyIn)
    };
    struct QuadVertex {
        float x, y, u, v;
    };
    struct Batch { // consecutive polygons sharing one clip rectangle
        uint32_t first, count;
        SDL_Rect clip;
    };

    SDL_GPUDevice *dev_ = nullptr;
    SDL_GPUGraphicsPipeline *poly_pipe_ = nullptr, *quad_pipe_ = nullptr, *back_pipe_ = nullptr, *front_pipe_ = nullptr;
    SDL_GPUSampler *sampler_ = nullptr;
    SDL_GPUTexture *front_ = nullptr; // the front layers from the CPU (HUD at the edges)
    SDL_GPUTexture *depth_ = nullptr;
    int depth_w_ = 0, depth_h_ = 0, front_w_ = 0, front_h_ = 0;
    SDL_GPUBuffer *vbuf_ = nullptr, *qbuf_ = nullptr;
    // storage buffers: texture RAM (both sheets), luma RAM, colour translation with gamma
    SDL_GPUBuffer *texram_ = nullptr, *luma_ = nullptr, *xlat_ = nullptr;
    OpponentBodies opponent_bodies_;
    SDL_GPUBuffer *opponent_atlas_ = nullptr;
    bool opponent_atlas_uploaded_ = false;
    uint64_t texram_generation_ = ~0ull; // the texture RAM generation texram_ holds
    // the tilemaps: pixmaps (4 x 512 x 512 u16), header + pens + tile words
    SDL_GPUBuffer *tilepix_ = nullptr, *tiledata_ = nullptr;
    uint64_t tile_instance_ = 0, tile_generation_ = 0; // the Video and its tile generation tilepix_ holds
    uint64_t sky_generation_ = 0; // the sky panorama tilepix_ holds (Video::sky_generation)
    struct Rows { uint32_t first, count; }; // pixmap rows (of pixels) to upload, per layer
    Rows tile_rows_[4];
    std::vector<uint32_t> tiledata_words_;
    std::vector<uint32_t> xlat_table_;
    uint32_t vbuf_size_ = 0;
    SDL_GPUTransferBuffer *upload_ = nullptr;
    uint32_t upload_size_ = 0;
    std::vector<PolyVertex> verts_;
    std::vector<Batch> batches_;
    std::vector<size_t> order_;
    uint8_t gamma_[256];
    std::string error_;

    SDL_GPUShader *shader(const unsigned char *spv, size_t spv_len, const unsigned char *dxil, size_t dxil_len,
                          const char *msl, const char *dxil_entry, SDL_GPUShaderStage stage, int uniforms, int samplers,
                          int storage_buffers = 0);
    bool ensure(int w, int h, int pixel_w, int pixel_h, uint32_t vert_bytes);
    void build(const rt::Video &video, int w, int h, int scale, bool sharp_textures,
               const std::vector<rt::GeoPoly> &polys);
};

} // namespace app
