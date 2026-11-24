#include "app/gpu/gpu_renderer.h"

#include "app/gpu/m2_polygon_material.h"
#include "app/gpu/shaders_gen.h"
#include "app/gpu/dxr_shaders_gen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace app {

namespace {

inline uint16_t le16(const uint8_t *base, uint32_t index) { return uint16_t(base[index * 2] | base[index * 2 + 1] << 8); }

constexpr uint32_t kSheetWords = 0x80000;                 // one texture sheet (VideoMem tex0, tex1)
constexpr uint32_t kTexramBytes = kSheetWords * 4 * 2;    // both sheets
constexpr uint32_t kLumaBytes = 0x20000;                  // luma RAM (one entry per dword)
constexpr uint32_t kXlatEntries = 0x6000;                 // colour translation (0xc000 bytes of u16)
constexpr uint32_t kTilePixBytes = 4 * 512 * 512 * 2;     // the four tilemap pixmaps, a u16 per pixel
constexpr uint32_t kSkyBytes = uint32_t(rt::Video::kSkyW) * rt::Video::kSkyH * 2; // the sky panorama, after them
constexpr uint32_t kTileHeader = 16;                      // m2.hlsl tiledata: header, pens, tile words
constexpr uint32_t kTileDataWords = kTileHeader + rt::Video::kGpuPens + rt::Video::kGpuTileWords / 2;

} // namespace

SDL_GPUShader *GpuRenderer::shader(const unsigned char *spv, size_t spv_len, const unsigned char *dxil, size_t dxil_len,
                                   const char *msl, const char *dxil_entry, SDL_GPUShaderStage stage, int uniforms,
                                   int samplers, int storage_buffers) {
    SDL_GPUShaderCreateInfo si{};
    si.stage = stage;
    si.num_uniform_buffers = Uint32(uniforms);
    si.num_samplers = Uint32(samplers);
    si.num_storage_buffers = Uint32(storage_buffers);
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev_);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        si.format = SDL_GPU_SHADERFORMAT_SPIRV, si.code = spv, si.code_size = spv_len, si.entrypoint = "main";
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        si.format = SDL_GPU_SHADERFORMAT_DXIL, si.code = dxil, si.code_size = dxil_len, si.entrypoint = dxil_entry;
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        si.format = SDL_GPU_SHADERFORMAT_MSL, si.code = reinterpret_cast<const Uint8 *>(msl),
        si.code_size = std::strlen(msl), si.entrypoint = "main0";
    } else {
        error_ = "no supported shader format (SPIR-V, DXIL or MSL)";
        return nullptr;
    }
    SDL_GPUShader *s = SDL_CreateGPUShader(dev_, &si);
    if (!s) error_ = std::string("SDL_CreateGPUShader: ") + SDL_GetError();
    return s;
}

bool GpuRenderer::init(SDL_GPUDevice *dev, SDL_GPUTextureFormat format) {
    dev_ = dev;
    frame_format_ = format;
    for (int i = 0; i < 256; i++) // the rasterizer's gamma (MAME video_start)
        gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));

    SDL_GPUShader *vs_poly = shader(k_vs_poly_spv, sizeof k_vs_poly_spv, k_vs_poly_dxil, sizeof k_vs_poly_dxil,
                                    k_vs_poly_msl, "vs_poly", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader *ps_poly = shader(k_ps_poly_spv, sizeof k_ps_poly_spv, k_ps_poly_dxil, sizeof k_ps_poly_dxil,
                                    k_ps_poly_msl, "ps_poly", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 4);
    SDL_GPUShader *vs_quad = shader(k_vs_quad_spv, sizeof k_vs_quad_spv, k_vs_quad_dxil, sizeof k_vs_quad_dxil,
                                    k_vs_quad_msl, "vs_quad", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader *ps_quad = shader(k_ps_quad_spv, sizeof k_ps_quad_spv, k_ps_quad_dxil, sizeof k_ps_quad_dxil,
                                    k_ps_quad_msl, "ps_quad", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    SDL_GPUShader *ps_back = shader(k_ps_tiles_back_spv, sizeof k_ps_tiles_back_spv, k_ps_tiles_back_dxil,
                                    sizeof k_ps_tiles_back_dxil, k_ps_tiles_back_msl, "ps_tiles_back",
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
    SDL_GPUShader *ps_front = shader(k_ps_tiles_front_spv, sizeof k_ps_tiles_front_spv, k_ps_tiles_front_dxil,
                                     sizeof k_ps_tiles_front_dxil, k_ps_tiles_front_msl, "ps_tiles_front",
                                     SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
    if (!vs_poly || !ps_poly || !vs_quad || !ps_quad || !ps_back || !ps_front) {
        for (SDL_GPUShader *s : {vs_poly, ps_poly, vs_quad, ps_quad, ps_back, ps_front})
            if (s) SDL_ReleaseGPUShader(dev_, s);
        return false;
    }

    SDL_GPUColorTargetDescription color{};
    color.format = format;

    // Polygons: position, depth (draw order), 1/z u/z v/z, colour, texture state.
    SDL_GPUVertexBufferDescription pvb{};
    pvb.slot = 0;
    pvb.pitch = sizeof(PolyVertex);
    pvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute pattr[6] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(PolyVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, depth))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, Uint32(offsetof(PolyVertex, ooz))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, Uint32(offsetof(PolyVertex, r))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, Uint32(offsetof(PolyVertex, info))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, Uint32(offsetof(PolyVertex, more))},
    };
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs_poly;
    pi.fragment_shader = ps_poly;
    pi.vertex_input_state.vertex_buffer_descriptions = &pvb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = pattr;
    pi.vertex_input_state.num_vertex_attributes = 6;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = true;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS; // earlier in the order wins
    pi.target_info.color_target_descriptions = &color;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    pi.target_info.has_depth_stencil_target = true;
    poly_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);

    // Layers: a textured quad; no depth test.
    SDL_GPUVertexBufferDescription qvb{};
    qvb.slot = 0;
    qvb.pitch = sizeof(QuadVertex);
    qvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute qattr[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, u))},
    };
    SDL_GPUGraphicsPipelineCreateInfo qi{};
    qi.vertex_shader = vs_quad;
    qi.fragment_shader = ps_quad;
    qi.vertex_input_state.vertex_buffer_descriptions = &qvb;
    qi.vertex_input_state.num_vertex_buffers = 1;
    qi.vertex_input_state.vertex_attributes = qattr;
    qi.vertex_input_state.num_vertex_attributes = 2;
    qi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    qi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    qi.target_info.color_target_descriptions = &color;
    qi.target_info.num_color_targets = 1;
    qi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    qi.target_info.has_depth_stencil_target = true; // same pass as the polygons
    quad_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    // Tilemap layers: the same quad, composed per pixel.
    qi.fragment_shader = ps_back;
    back_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    qi.fragment_shader = ps_front;
    front_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);

    for (SDL_GPUShader *s : {vs_poly, ps_poly, vs_quad, ps_quad, ps_back, ps_front}) SDL_ReleaseGPUShader(dev_, s);
    if (!poly_pipe_ || !quad_pipe_ || !back_pipe_ || !front_pipe_) {
        error_ = std::string("SDL_CreateGPUGraphicsPipeline: ") + SDL_GetError();
        shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo smp{}; // layers are drawn 1:1: nearest
    smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_NEAREST;
    smp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(dev_, &smp);

    SDL_GPUBufferCreateInfo qb{};
    qb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    qb.size = sizeof(QuadVertex) * 6;
    qbuf_ = SDL_CreateGPUBuffer(dev_, &qb);
    SDL_GPUBufferCreateInfo sb{};
    sb.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    sb.size = kTexramBytes;
    texram_ = SDL_CreateGPUBuffer(dev_, &sb);
    if (const char *directory = std::getenv("M2_OPPONENT_BODIES"); directory && *directory) {
        if (opponent_bodies_.load(directory)) {
            sb.size = OpponentBodies::atlas_bytes;
            opponent_atlas_ = SDL_CreateGPUBuffer(dev_, &sb);
        }
        std::fprintf(stderr, "daytona: local opponent bodies %s\n",
                     opponent_atlas_ ? "loaded" : "unavailable, keeping original bodies");
    }
    sb.size = kLumaBytes;
    luma_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kXlatEntries * 4;
    xlat_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kTilePixBytes + kSkyBytes;
    tilepix_ = SDL_CreateGPUBuffer(dev_, &sb);
    sb.size = kTileDataWords * 4;
    tiledata_ = SDL_CreateGPUBuffer(dev_, &sb);
    if (!sampler_ || !qbuf_ || !texram_ || !luma_ || !xlat_ || !tilepix_ || !tiledata_) {
        error_ = std::string("SDL_GPU setup: ") + SDL_GetError();
        shutdown();
        return false;
    }
    return true;
}

void GpuRenderer::shutdown() {
    if (!dev_) return;
    if (poly_pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, poly_pipe_), poly_pipe_ = nullptr;
    for (SDL_GPUGraphicsPipeline **p : {&quad_pipe_, &back_pipe_, &front_pipe_})
        if (*p) SDL_ReleaseGPUGraphicsPipeline(dev_, *p), *p = nullptr;
    if (sampler_) SDL_ReleaseGPUSampler(dev_, sampler_), sampler_ = nullptr;
    for (SDL_GPUGraphicsPipeline **p : {&z_pipe_, &z_front_pipe_, &z_quad_pipe_, &shade_pipe_, &car_z_pipe_})
        if (*p) SDL_ReleaseGPUGraphicsPipeline(dev_, *p), *p = nullptr;
    if (view_z_) SDL_ReleaseGPUTexture(dev_, view_z_), view_z_ = nullptr;
    if (light_) SDL_ReleaseGPUTexture(dev_, light_), light_ = nullptr;
    shadows_ = shadow_ready_ = false;
    shadow_frame_ = ShadowFrame{};
    if (front_) SDL_ReleaseGPUTexture(dev_, front_), front_ = nullptr;
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_), depth_ = nullptr;
    if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_), vbuf_ = nullptr;
    if (qbuf_) SDL_ReleaseGPUBuffer(dev_, qbuf_), qbuf_ = nullptr;
    for (SDL_GPUBuffer **b : {&texram_, &luma_, &xlat_, &tilepix_, &tiledata_, &opponent_atlas_})
        if (*b) SDL_ReleaseGPUBuffer(dev_, *b), *b = nullptr;
    texram_generation_ = ~0ull;
    opponent_atlas_uploaded_ = false;
    tile_instance_ = 0;
    if (upload_) SDL_ReleaseGPUTransferBuffer(dev_, upload_), upload_ = nullptr;
    vbuf_size_ = upload_size_ = 0;
    depth_w_ = depth_h_ = front_w_ = front_h_ = 0;
}

// Textures and buffers big enough for this frame.
bool GpuRenderer::ensure(int w, int h, int pixel_w, int pixel_h, uint32_t vert_bytes) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    if (w != front_w_ || h != front_h_) {
        if (front_) SDL_ReleaseGPUTexture(dev_, front_), front_ = nullptr;
        ti.width = Uint32(w);
        ti.height = Uint32(h);
        ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the layers' 0xAARRGGBB words
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        front_ = SDL_CreateGPUTexture(dev_, &ti);
        if (!front_) return false;
        front_w_ = w, front_h_ = h;
    }
    if (pixel_w != depth_w_ || pixel_h != depth_h_) {
        if (depth_) SDL_ReleaseGPUTexture(dev_, depth_), depth_ = nullptr;
        ti.width = Uint32(pixel_w);
        ti.height = Uint32(pixel_h);
        ti.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        depth_ = SDL_CreateGPUTexture(dev_, &ti);
        if (!depth_) return false;
        depth_w_ = pixel_w, depth_h_ = pixel_h;
    }
    if (vert_bytes > vbuf_size_) {
        if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_);
        vbuf_size_ = std::max<uint32_t>(vert_bytes, 1u << 20) * 2;
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = vbuf_size_;
        vbuf_ = SDL_CreateGPUBuffer(dev_, &bi);
        if (!vbuf_) return false;
    }
    const uint32_t need = uint32_t(w) * uint32_t(h) * 4 + uint32_t(sizeof(QuadVertex) * 6) + vert_bytes +
                          kTexramBytes + kLumaBytes + kXlatEntries * 4 + kTilePixBytes + kSkyBytes + kTileDataWords * 4 + (opponent_atlas_ && !opponent_atlas_uploaded_ ? OpponentBodies::atlas_bytes : 0);
    if (need > upload_size_) {
        if (upload_) SDL_ReleaseGPUTransferBuffer(dev_, upload_);
        upload_size_ = need * 2;
        SDL_GPUTransferBufferCreateInfo tb{};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = upload_size_;
        upload_ = SDL_CreateGPUTransferBuffer(dev_, &tb);
        if (!upload_) return false;
    }
    return true;
}

// The polygons as triangles, in the rasterizer's order, batched by clip rectangle.
void GpuRenderer::build(const rt::Video &video, int w, int h, int scale, bool sharp_textures,
                        const std::vector<rt::GeoPoly> &polys) {
    verts_.clear();
    batches_.clear();
    if (triangles_) triangles_->clear();
    cameras_.clear();
    shadow_polys_.clear();
    z_draws_.clear();
    car_z_draws_.clear();
    cutout_xyz_.clear();
    cutout_records_.clear();
    main_camera_ = car_camera_ = ~0u;
    const rt::VideoMem &mem = video.gpu_mem();
    const int windows = video.gpu_windows();
    // widescreen: the screen is `margin` wider on each side; x shifts by it
    const int margin = (w - rt::Video::W) / 2;
    const int crtc_x = video.crtc_x() + margin, crtc_y = video.crtc_y(), render_x = video.render_x() + margin,
              render_y = video.render_y();
    // HUD at the edges: the condition panel's own quads (its z, inside its box) move
    const int hud_dx = video.gpu_hud_shift();
    const float *hud_box = video.raster().hud_box();
    const uint16_t hud_z = video.raster().hud_z();
    if (polys.empty() || !mem.palram || !mem.colorxlat) return;
    // internal resolution: the mip level the rasterizer picks from z, less
    // log2(scale) (its levels are 128 units of fast_log2)
    const int lod_bias = int(std::lround(128.0 * std::log2(double(scale))));

    // Raster::render: windows from the last down to 0, low z first, newest first within z.
    order_.resize(polys.size());
    std::iota(order_.begin(), order_.end(), size_t(0));
    std::sort(order_.begin(), order_.end(), [&](size_t a, size_t b) {
        const auto &left = polys[a], &right = polys[b];
        if (left.window != right.window) return left.window > right.window;
        if (left.z != right.z) return left.z < right.z;
        return a > b;
    });
    const float depth_step = 1.0f / float(order_.size() + 2);
    uint32_t drawn = 0;
    for (size_t i : order_) {
        const rt::GeoPoly &poly = polys[i];
        if (poly.window > windows || poly.num_vertices < 3) continue;
        const int renderer = (poly.texheader[0] >> 13) & 3;
        if (renderer == 1) continue; // translucent solid: the rasterizer draws nothing
        // Diagnostic (3D tree study): cut-out textured scenery polygons
        // (renderer 3, a ROM object, not a car part). M2_CUTOUT_TRACE=FILE logs
        // one line per such polygon (object, texture words, object-space
        // extent, view-space centre); M2_HIDE_CUTOUT=1 skips them, to see
        // what they are on screen.
        if (renderer == 3 && (poly.object_oba & 0x01800000u) == 0x00800000u && !poly.mobile_key.part) {
            static std::FILE *cut_trace = [] {
                const char *p = std::getenv("M2_CUTOUT_TRACE");
                return p && *p ? std::fopen(p, "w") : nullptr;
            }();
            static const bool hide_cut = [] {
                const char *v = std::getenv("M2_HIDE_CUTOUT");
                return v && v[0] == '1';
            }();
            if (cut_trace) {
                float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                for (int k = 0; k < poly.source_num_vertices; ++k)
                    for (int a = 0; a < 3; ++a)
                        lo[a] = std::min(lo[a], poly.source_v[k][a]), hi[a] = std::max(hi[a], poly.source_v[k][a]);
                float cx = 0, cy = 0, cz = 0;
                for (int k = 0; k < poly.num_vertices; ++k) cx += poly.v[k].x, cy += poly.v[k].y, cz += poly.v[k].p[0];
                const float nv = float(std::max<int>(poly.num_vertices, 1));
                std::fprintf(cut_trace, "%x,%04x,%04x,%04x,%04x,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.5f\n",
                             poly.object_oba, poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.texheader[3],
                             lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], cx / nv, cy / nv, cz / nv);
            }
            if (hide_cut) continue;
            // M2_HIDE_CUTOUT_TEX="w2:w3,w2:w3" (hex texture header words 2 and 3):
            // skip only those textures, to tell which ones are the trees.
            static const std::vector<uint32_t> hide_tex = [] {
                std::vector<uint32_t> keys;
                if (const char *v = std::getenv("M2_HIDE_CUTOUT_TEX"))
                    for (const char *p = v; *p;) {
                        char *end = nullptr;
                        const unsigned long w2 = std::strtoul(p, &end, 16);
                        if (!end || *end != ':') break;
                        const unsigned long w3 = std::strtoul(end + 1, &end, 16);
                        keys.push_back(uint32_t(w2 << 16 | (w3 & 0xffff)));
                        p = (*end == ',') ? end + 1 : end;
                    }
                return keys;
            }();
            if (!hide_tex.empty() &&
                std::find(hide_tex.begin(), hide_tex.end(), uint32_t(poly.texheader[2]) << 16 | poly.texheader[3]) != hide_tex.end())
                continue;
        }

        // HUD overlay quad? (Raster::render_one's test, on the projected outline)
        int dx = 0;
        if (hud_dx && poly.z == hud_z) {
            float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
            for (int k = 0; k < poly.num_vertices; k++) {
                const rt::GeoVertex &g0 = poly.v[k];
                const float z = g0.p[0] + std::numeric_limits<float>::min();
                const float px = float(crtc_x + poly.center[0]) + g0.x / z - float(margin);
                const float py = float((384 - poly.center[1]) + crtc_y) - g0.y / z;
                bx0 = std::min(bx0, px), bx1 = std::max(bx1, px), by0 = std::min(by0, py), by1 = std::max(by1, py);
            }
            constexpr float kTol = 1.5f;
            if (bx0 >= hud_box[0] - kTol && bx1 <= hud_box[1] + kTol && by0 >= hud_box[2] - kTol && by1 <= hud_box[3] + kTol)
                dx = hud_dx;
        }
        // clip rectangle (Raster::render_one), in the renderer's offsets, within the screen;
        // a viewport spanning the screen extends into the widescreen margins
        const int wide = margin && poly.viewport[0] <= 0 && poly.viewport[2] >= 495 ? margin : 0;
        const int x0 = std::max(poly.viewport[0] - wide + render_x + dx, 0),
                  x1 = std::min(poly.viewport[2] + wide + render_x + dx, w - 1);
        const int y0 = std::max((384 - poly.viewport[3]) + render_y, 0), y1 = std::min((384 - poly.viewport[1]) + render_y, h - 1);
        if (x0 > x1 || y0 > y1) continue;
        const SDL_Rect clip{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        if (batches_.empty() || std::memcmp(&batches_.back().clip, &clip, sizeof clip) != 0)
            batches_.push_back({uint32_t(verts_.size()), 0, clip});

        // colour: the solid renderer's (palette entry, luma translation, gamma)
        const uint32_t luma = poly.luma >> 2;
        const uint16_t color = poly.local_solid_palette ? poly.local_solid_palette :
            le16(mem.palram, m2gpu::polygon_palette_index(poly.texheader[3]));
        const float r = gamma_[le16(mem.colorxlat, 0x0000 / 2 + (((color >> 0) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float depth = float(++drawn) * depth_step;

        // texture state, decoded as render_one (m2.hlsl PolyIn)
        auto material = m2gpu::pack_polygon_material(
            poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.luma, poly.texlod, lod_bias, color);
        if (sharp_textures) material.info[0] |= 1u << 23;
        if (poly.local_texture) {
            material.info[0] = 1u << 24;
            material.info[1] = poly.local_texture - 1;
            material.info[2] = poly.luma;
        }

        // model2_3d_project, then a fan from vertex 0 (the polygon is convex)
        PolyVertex v[8];
        for (int k = 0; k < poly.num_vertices; k++) {
            const rt::GeoVertex &g0 = poly.v[k];
            const float z = g0.p[0] + std::numeric_limits<float>::min();
            const float ooz = 1.0f / z; // render_one, textured: p[0] = 1/z, p[1..2] scaled by it / 8
            v[k] = {float(crtc_x + dx + poly.center[0]) + g0.x / z, float((384 - poly.center[1]) + crtc_y) - g0.y / z, depth,
                    ooz, g0.p[1] * ooz * (1.0f / 8.0f), g0.p[2] * ooz * (1.0f / 8.0f),
                    r, g, b, 1.0f, {material.info[0], material.info[1], material.info[2], material.info[3]},
                    {material.more[0], material.more[1]}};
        }
        if (triangles_ && i < polys.size()) {
            // The shadows' camera is the most common one (window, centre,
            // focal length); polygons drawn through another (the 3D minimap
            // in its own window) neither cast nor receive shadows.
            bool in_front = true;
            for (int k = 0; k < poly.num_vertices; k++) in_front = in_front && poly.v[k].p[0] > 0.0f;
            uint32_t camera_index = ~0u;
            if (in_front && dx == 0 && !rt::Geo::hud_overlay_address(poly.object_oba)) {
                const std::array<float, 5> camera{float(crtc_x + poly.center[0]), float((384 - poly.center[1]) + crtc_y),
                                                  poly.object_focus_x, poly.object_focus_y, float(poly.window)};
                auto it = std::find_if(cameras_.begin(), cameras_.end(), [&](const auto &c) { return c.first == camera; });
                if (it == cameras_.end()) it = cameras_.insert(cameras_.end(), {camera, 0});
                ++it->second;
                camera_index = uint32_t(it - cameras_.begin());
            }
            // Cut-out textures (renderer 3: fir trees, fences) go apart, with
            // their texture coordinates, so a ray passes through the holes.
            const bool cutout = renderer == 3 && !poly.local_texture;
            std::vector<float> &xyz = cutout ? cutout_xyz_ : *triangles_;
            const uint32_t tri_first = uint32_t(xyz.size() / 9);
            for (int k = 1; camera_index != ~0u && k + 1 < poly.num_vertices; k++) {
                if (cutout) {
                    cutout_records_.insert(cutout_records_.end(), {material.info[0], material.info[1]});
                    for (int corner : {0, k, k + 1}) // u, v as m2.hlsl ps_poly reads them: p[1..2] / 8 * 256
                        for (float t : {poly.v[corner].p[1] * 32.0f, poly.v[corner].p[2] * 32.0f}) {
                            uint32_t bits;
                            std::memcpy(&bits, &t, 4);
                            cutout_records_.push_back(bits);
                        }
                }
                for (int corner : {0, k, k + 1}) {
                    const rt::GeoVertex &g = poly.v[corner];
                    xyz.insert(xyz.end(), {g.x, g.y, g.p[0]});
                }
            }
            shadow_polys_.push_back({uint32_t(verts_.size()), uint32_t(3 * (poly.num_vertices - 2)),
                                     uint32_t(batches_.size() - 1), camera_index, tri_first,
                                     uint32_t(xyz.size() / 9) - tri_first, cutout});
        }
        for (int k = 1; k + 1 < poly.num_vertices; k++) {
            verts_.push_back(v[0]);
            verts_.push_back(v[k]);
            verts_.push_back(v[k + 1]);
        }
        batches_.back().count = uint32_t(verts_.size()) - batches_.back().first;
    }
    if (!triangles_) return;
    // Keep the main camera's polygons: their triangles for the acceleration
    // structure, their vertex runs (per clip batch) for the view z pass.
    const auto main = std::max_element(cameras_.begin(), cameras_.end(),
                                       [](const auto &a, const auto &b) { return a.second < b.second; });
    main_camera_ = main == cameras_.end() ? ~0u : uint32_t(main - cameras_.begin());
    // Each camera's part: 1 the scenery's (the main camera, or one that only
    // differs from it by a hair of focal length: the zoom at the start of a
    // race splits the scenery between two, 242.71 and 243.04 on Expert, 23/11,
    // and up to 1200 polygons, the start gantry among them, had no shadow),
    // 2 the player's car, 0 none. The scale brings a camera's triangles to
    // the main focal length.
    std::vector<uint8_t> part(cameras_.size(), 0);
    std::vector<std::array<float, 2>> rescale(cameras_.size(), {1.0f, 1.0f});
    if (main_camera_ != ~0u) {
        const auto &m = cameras_[main_camera_].first;
        for (size_t c = 0; c < cameras_.size(); ++c) {
            const auto &k = cameras_[c].first;
            if (k[2] <= 0.0f || k[3] <= 0.0f) continue;
            rescale[c] = {m[2] / k[2], m[3] / k[3]};
            if (c == main_camera_ || (k[4] == m[4] && k[0] == m[0] && k[1] == m[1] &&
                                      std::fabs(rescale[c][0] - 1.0f) < 0.02f && std::fabs(rescale[c][1] - 1.0f) < 0.02f))
                part[c] = 1;
        }
    }
    // The player's car: the game draws it through its own camera, in the
    // same window but with another centre and focal length (Expert, 23/11:
    // centre y 196 against 242, focal length 280 against 210, 76 polygons).
    // The same eye, so its points are the same in the true camera space:
    // its triangles go into the structure rescaled to the main focal length
    // (it casts its shadow), its view z is drawn negated (it receives them).
    if (main_camera_ != ~0u) {
        const auto &m = cameras_[main_camera_];
        uint32_t best = 0;
        for (size_t c = 0; c < cameras_.size(); ++c)
            if (part[c] == 0 && cameras_[c].first[4] == m.first[4] && cameras_[c].first[0] == m.first[0] &&
                cameras_[c].first[2] > 0.0f && cameras_[c].first[3] > 0.0f && cameras_[c].second > best)
                best = cameras_[c].second, car_camera_ = uint32_t(c);
        if (car_camera_ != ~0u) part[car_camera_] = 2;
    }
    // Moved down in place (a triangle never moves up), rescaled to the main
    // focal length.
    auto keep = [](std::vector<float> &xyz, size_t to, uint32_t from, uint32_t count, std::array<float, 2> f) {
        for (uint32_t t = 0; t < 3 * count; ++t) {
            const float *v = xyz.data() + 3 * (from * 3 + t);
            float *d = xyz.data() + 3 * (to * 3 + t);
            d[0] = v[0] * f[0], d[1] = v[1] * f[1], d[2] = v[2];
        }
    };
    auto add_run = [](std::vector<ZDraw> &draws, const ShadowPoly &s) {
        if (!draws.empty() && draws.back().batch == s.batch && draws.back().first + draws.back().count == s.first)
            draws.back().count += s.count;
        else
            draws.push_back({s.first, s.count, s.batch});
    };
    size_t kept = 0, kept_cut = 0; // in triangles
    for (const ShadowPoly &s : shadow_polys_) {
        const uint8_t role = s.camera < part.size() ? part[s.camera] : 0;
        if (role == 0 || (role == 2 && s.cutout)) continue;
        if (s.cutout) {
            keep(cutout_xyz_, kept_cut, s.tri_first, s.tri_count, rescale[s.camera]);
            if (kept_cut != s.tri_first)
                std::copy_n(cutout_records_.begin() + 8 * s.tri_first, 8 * s.tri_count,
                            cutout_records_.begin() + 8 * kept_cut);
            kept_cut += s.tri_count;
        } else {
            keep(*triangles_, kept, s.tri_first, s.tri_count, rescale[s.camera]);
            kept += s.tri_count;
        }
        add_run(role == 2 ? car_z_draws_ : z_draws_, s);
    }
    triangles_->resize(9 * kept);
    cutout_xyz_.resize(9 * kept_cut);
    cutout_records_.resize(8 * kept_cut);
    // The shadow casters behind the camera, through the main camera only:
    // in the acceleration structure, not in the view z pass.
    if (casters_ && main_camera_ != ~0u) {
        const auto &main_key = cameras_[main_camera_].first;
        for (const rt::GeoPoly &poly : *casters_) {
            if (poly.num_vertices < 3) continue;
            const int renderer = (poly.texheader[0] >> 13) & 3;
            if (renderer == 1) continue;
            const std::array<float, 5> camera{float(crtc_x + poly.center[0]), float((384 - poly.center[1]) + crtc_y),
                                              poly.object_focus_x, poly.object_focus_y, float(poly.window)};
            if (camera[0] != main_key[0] || camera[1] != main_key[1] || camera[4] != main_key[4] ||
                camera[2] <= 0.0f || camera[3] <= 0.0f || rt::Geo::hud_overlay_address(poly.object_oba))
                continue;
            const float fx = main_key[2] / camera[2], fy = main_key[3] / camera[3];
            if (std::fabs(fx - 1.0f) >= 0.02f || std::fabs(fy - 1.0f) >= 0.02f) continue;
            const bool cutout = renderer == 3 && !poly.local_texture;
            std::vector<float> &xyz = cutout ? cutout_xyz_ : *triangles_;
            if (cutout) {
                const uint16_t color = le16(mem.palram, m2gpu::polygon_palette_index(poly.texheader[3]));
                const auto material = m2gpu::pack_polygon_material(
                    poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.luma, poly.texlod, lod_bias, color);
                for (int k = 1; k + 1 < poly.num_vertices; k++) {
                    cutout_records_.insert(cutout_records_.end(), {material.info[0], material.info[1]});
                    for (int corner : {0, k, k + 1})
                        for (float t : {poly.v[corner].p[1] * 32.0f, poly.v[corner].p[2] * 32.0f}) {
                            uint32_t bits;
                            std::memcpy(&bits, &t, 4);
                            cutout_records_.push_back(bits);
                        }
                }
            }
            for (int k = 1; k + 1 < poly.num_vertices; k++)
                for (int corner : {0, k, k + 1}) {
                    const rt::GeoVertex &g = poly.v[corner];
                    xyz.insert(xyz.end(), {g.x * fx, g.y * fy, g.p[0]});
                }
        }
    }
}

bool GpuRenderer::render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h,
                         const rt::Video &video, int scale, int pixel_w, int pixel_h,
                         bool sharp_textures, const std::vector<rt::GeoPoly> *pose_polys) {
    if (!ok()) return false;
    scale = std::clamp(scale, 1, 4);
    if (pixel_w <= 0 || pixel_h <= 0) {
        pixel_w = w * scale;
        pixel_h = h * scale;
    }
    // The 2x buffer resolves to its first mip. Select texture LOD for the
    // output pixels, and use the finer buffer only for coverage.
    const int output_h = sharp_textures ? pixel_h / 2 : pixel_h;
    const int lod_scale = std::clamp(int(std::lround(double(output_h) / h)), 1, 4);
    const auto &original = pose_polys ? *pose_polys : video.gpu_polys();
    const auto *replacement = opponent_atlas_ ? opponent_bodies_.replace(original) : nullptr;
    build(video, w, h, lod_scale, sharp_textures, replacement ? *replacement : original);
    static std::FILE *trace = [] {
        const char *file = std::getenv("M2_OPPONENT_PRESENT_TRACE");
        if (!file || !*file) return static_cast<std::FILE *>(nullptr);
        auto *output = std::fopen(file, "w");
        if (output) std::fputs("time_ns,original_faces,presented_faces,vertices,replaced\n", output);
        return output;
    }();
    if (trace) {
        std::fprintf(trace, "%llu,%zu,%zu,%zu,%d\n",
                     static_cast<unsigned long long>(SDL_GetTicksNS()), original.size(),
                     replacement ? replacement->size() : original.size(), verts_.size(), replacement != nullptr);
    }
    const uint32_t vert_bytes = uint32_t(verts_.size() * sizeof(PolyVertex));
    if (!ensure(w, h, pixel_w, pixel_h, vert_bytes)) return false;

    // The tilemap pixmaps' rows to upload, per layer: the span of the tile
    // rows changed since tilepix_ was filled (everything for a new Video).
    const uint64_t tile_generation = video.system24_texture_generation();
    const bool all_tiles = video.instance() != tile_instance_ || tile_generation < tile_generation_;
    for (int l = 0; l < 4; l++) {
        int first = 64, last = -1;
        for (unsigned r = 0; r < 64; r++) {
            bool changed = all_tiles;
            for (unsigned t = r * 64; !changed && t < r * 64 + 64; t++)
                changed = video.system24_tile_generation(l, t) > tile_generation_;
            if (changed) first = std::min(first, int(r)), last = int(r);
        }
        tile_rows_[l] = last < 0 ? Rows{0, 0} : Rows{uint32_t(first) * 8, uint32_t(last - first + 1) * 8};
    }
    tile_instance_ = video.instance();
    tile_generation_ = tile_generation;

    // upload: the CPU front layers (HUD at the edges only), the layer quad,
    // the polygon vertices, the storage data
    const bool cpu_front = video.cpu_front();
    const uint32_t layer_bytes = uint32_t(w) * uint32_t(h) * 4;
    auto *p = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev_, upload_, true));
    uint32_t at = 0;
    if (cpu_front) {
        const std::vector<uint32_t> &front = video.foreground_layer();
        std::memcpy(p, front.data(), std::min(front.size() * 4, size_t(layer_bytes)));
        at = layer_bytes;
    }
    const float fw = float(w), fh = float(h);
    const QuadVertex quad[6] = {{0, 0, 0, 0}, {fw, 0, 1, 0}, {0, fh, 0, 1}, {fw, 0, 1, 0}, {fw, fh, 1, 1}, {0, fh, 0, 1}};
    const uint32_t quad_at = at;
    std::memcpy(p + quad_at, quad, sizeof quad);
    const uint32_t verts_at = quad_at + uint32_t(sizeof quad);
    if (vert_bytes) std::memcpy(p + verts_at, verts_.data(), vert_bytes);
    at = verts_at + vert_bytes;
    // luma RAM and the gamma'd colour translation every frame, texture RAM when it changed
    const rt::VideoMem &mem = video.gpu_mem();
    const uint32_t data_at = at;
    const bool have_mem = mem.lumaram && mem.colorxlat && mem.tex0 && mem.tex1;
    bool tex_changed = false;
    if (have_mem) {
        std::memcpy(p + data_at, mem.lumaram, kLumaBytes);
        xlat_table_.resize(kXlatEntries);
        for (uint32_t i = 0; i < kXlatEntries; i++) xlat_table_[i] = gamma_[le16(mem.colorxlat, i) & 0xff];
        std::memcpy(p + data_at + kLumaBytes, xlat_table_.data(), kXlatEntries * 4);
        at += kLumaBytes + kXlatEntries * 4;
        tex_changed = mem.tex_generation != texram_generation_; // the board counts texture RAM writes
        if (tex_changed) {
            texram_generation_ = mem.tex_generation;
            std::memcpy(p + at, mem.tex0, kSheetWords * 4);
            std::memcpy(p + at + kSheetWords * 4, mem.tex1, kSheetWords * 4);
            at += kTexramBytes;
        }
    }
    // tilemaps: the changed pixmap rows, packed as m2.hlsl reads them (pen, category in bit 15)
    uint32_t rows_at[4];
    for (int l = 0; l < 4; l++) {
        rows_at[l] = at;
        const uint32_t n = tile_rows_[l].count * 512;
        if (!n) continue;
        const uint16_t *pixels = video.system24_pixels(l) + size_t(tile_rows_[l].first) * 512;
        const uint8_t *flags = video.system24_flags(l) + size_t(tile_rows_[l].first) * 512;
        uint16_t *dst = reinterpret_cast<uint16_t *>(p + at);
        for (uint32_t i = 0; i < n; i++) dst[i] = uint16_t(pixels[i] | (flags[i] & 1) << 15);
        at += n * 2;
    }
    // the sky panorama, when the matched one changed (or for a new Video)
    const uint32_t sky_at = at;
    const uint16_t *sky = video.sky_panorama();
    const bool sky_upload = sky && (all_tiles || video.sky_generation() != sky_generation_);
    if (sky_upload) {
        std::memcpy(p + sky_at, sky, kSkyBytes);
        at += kSkyBytes;
        sky_generation_ = video.sky_generation();
    }
    // the header (widescreen margin and its fill), pens, tile words
    tiledata_words_.assign(kTileDataWords, 0);
    tiledata_words_[0] = uint32_t(video.margin());
    tiledata_words_[1] = uint32_t(video.backdrop()); // Edges 0, Sky 1, Stretch 2, as m2.hlsl
    tiledata_words_[2] = uint32_t(scale);
    tiledata_words_[3] = uint32_t(pixel_w);
    tiledata_words_[4] = uint32_t(pixel_h);
    tiledata_words_[5] = uint32_t(video.sky_origin());
    std::memcpy(&tiledata_words_[kTileHeader], video.gpu_pens(), rt::Video::kGpuPens * 4);
    std::memcpy(&tiledata_words_[kTileHeader + rt::Video::kGpuPens], video.gpu_tile_words(), rt::Video::kGpuTileWords * 2);
    const uint32_t tiledata_at = at;
    std::memcpy(p + tiledata_at, tiledata_words_.data(), kTileDataWords * 4);
    const bool upload_opponents = opponent_atlas_ && !opponent_atlas_uploaded_;
    const uint32_t opponent_at = tiledata_at + kTileDataWords * 4;
    if (upload_opponents)
        std::memcpy(p + opponent_at, opponent_bodies_.atlas().data(), OpponentBodies::atlas_bytes);
    SDL_UnmapGPUTransferBuffer(dev_, upload_);

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    // cycle: the whole buffer is replaced, so a buffer still in use may be swapped for a fresh one
    auto upload_buffer = [&](uint32_t from, SDL_GPUBuffer *buffer, uint32_t offset, uint32_t size, bool cycle = true) {
        SDL_GPUTransferBufferLocation src{upload_, from};
        SDL_GPUBufferRegion dst{buffer, offset, size};
        SDL_UploadToGPUBuffer(copy, &src, &dst, cycle);
    };
    if (cpu_front) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = upload_;
        src.pixels_per_row = Uint32(w);
        SDL_GPUTextureRegion dst{};
        dst.texture = front_;
        dst.w = Uint32(w);
        dst.h = Uint32(h);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
    }
    upload_buffer(quad_at, qbuf_, 0, sizeof quad);
    if (vert_bytes) upload_buffer(verts_at, vbuf_, 0, vert_bytes);
    if (have_mem) {
        upload_buffer(data_at, luma_, 0, kLumaBytes);
        upload_buffer(data_at + kLumaBytes, xlat_, 0, kXlatEntries * 4);
        if (tex_changed) upload_buffer(data_at + kLumaBytes + kXlatEntries * 4, texram_, 0, kTexramBytes);
    }
    for (int l = 0; l < 4; l++)
        if (tile_rows_[l].count)
            upload_buffer(rows_at[l], tilepix_, (uint32_t(l) * 512 * 512 + tile_rows_[l].first * 512) * 2,
                          tile_rows_[l].count * 512 * 2, false); // rows: the rest must stay
    if (sky_upload) upload_buffer(sky_at, tilepix_, kTilePixBytes, kSkyBytes, false);
    upload_buffer(tiledata_at, tiledata_, 0, kTileDataWords * 4);
    if (upload_opponents) {
        upload_buffer(opponent_at, opponent_atlas_, 0, OpponentBodies::atlas_bytes);
        opponent_atlas_uploaded_ = true;
    }
    SDL_EndGPUCopyPass(copy);

    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = SDL_FColor{0, 0, 0, 1};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth_;
    dt.clear_depth = 1.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    shadow_ready_ = false;
    const bool shadow_inputs = shadows_ && !verts_.empty() && ensure_shadow_targets(pixel_w, pixel_h);
    dt.store_op = shadow_inputs ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE; // the z pass reads it
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    if (!pass) return false;
    // the target may be bigger than this frame; positions are in original
    // pixels (screen), the viewport and clip rectangles in the target's
    const SDL_GPUViewport viewport{0, 0, float(pixel_w), float(pixel_h), 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    const float screen[4] = {fw, fh, 0, 0};
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    const SDL_Rect full{0, 0, pixel_w, pixel_h};

    // a full-frame quad through `pipe`: the tilemap layers, or the CPU front layers
    auto draw_quad = [&](SDL_GPUGraphicsPipeline *pipe) {
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        SDL_SetGPUScissor(pass, &full);
        SDL_GPUBufferBinding qb{qbuf_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &qb, 1);
        if (pipe == quad_pipe_) {
            SDL_GPUTextureSamplerBinding tsb{front_, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        } else {
            SDL_GPUBuffer *storage[2] = {tilepix_, tiledata_};
            SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 2);
        }
        SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    };
    draw_quad(back_pipe_);
    if (!verts_.empty()) {
        SDL_BindGPUGraphicsPipeline(pass, poly_pipe_);
        SDL_GPUBufferBinding vb{vbuf_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUBuffer *storage[4] = {texram_, luma_, xlat_, opponent_atlas_ ? opponent_atlas_ : texram_};
        SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 4);
        for (const Batch &b : batches_) {
            if (!b.count) continue;
            const int x0 = b.clip.x * pixel_w / w;
            const int y0 = b.clip.y * pixel_h / h;
            const int x1 = (b.clip.x + b.clip.w) * pixel_w / w;
            const int y1 = (b.clip.y + b.clip.h) * pixel_h / h;
            if (x1 <= x0 || y1 <= y0) continue;
            const SDL_Rect clip{x0, y0, x1 - x0, y1 - y0};
            SDL_SetGPUScissor(pass, &clip);
            SDL_DrawGPUPrimitives(pass, b.count, 1, b.first, 0);
        }
    }
    draw_quad(cpu_front ? quad_pipe_ : front_pipe_);
    SDL_EndGPURenderPass(pass);
    if (!shadow_inputs) return true;

    // Shadow inputs (dxr.hlsl step 1): the view z of the polygon that won
    // each pixel, by the depth test EQUAL against the pass above; 0 where a
    // front layer covers the 3D, or where nothing was drawn.
    SDL_GPUColorTargetInfo zt{};
    zt.texture = view_z_;
    zt.load_op = SDL_GPU_LOADOP_CLEAR;
    zt.clear_color = SDL_FColor{0, 0, 0, 0};
    zt.store_op = SDL_GPU_STOREOP_STORE;
    dt.load_op = SDL_GPU_LOADOP_LOAD;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    pass = SDL_BeginGPURenderPass(cmd, &zt, 1, &dt);
    if (!pass) return true; // the frame itself is drawn
    SDL_SetGPUViewport(pass, &viewport);
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    SDL_BindGPUGraphicsPipeline(pass, z_pipe_);
    SDL_GPUBufferBinding vb{vbuf_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    auto draw_z = [&](const std::vector<ZDraw> &draws) {
        for (const ZDraw &d : draws) {
            const Batch &b = batches_[d.batch];
            const int x0 = b.clip.x * pixel_w / w;
            const int y0 = b.clip.y * pixel_h / h;
            const int x1 = (b.clip.x + b.clip.w) * pixel_w / w;
            const int y1 = (b.clip.y + b.clip.h) * pixel_h / h;
            if (x1 <= x0 || y1 <= y0) continue;
            const SDL_Rect clip{x0, y0, x1 - x0, y1 - y0};
            SDL_SetGPUScissor(pass, &clip);
            SDL_DrawGPUPrimitives(pass, d.count, 1, d.first, 0);
        }
    };
    draw_z(z_draws_); // the main camera's polygons
    if (car_z_pipe_ && !car_z_draws_.empty()) { // the player's car: negative view z
        SDL_BindGPUGraphicsPipeline(pass, car_z_pipe_);
        draw_z(car_z_draws_);
    }
    if (cpu_front) {
        SDL_BindGPUGraphicsPipeline(pass, z_quad_pipe_);
        SDL_GPUTextureSamplerBinding tsb{front_, sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
    } else {
        SDL_BindGPUGraphicsPipeline(pass, z_front_pipe_);
        SDL_GPUBuffer *storage[2] = {tilepix_, tiledata_};
        SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 2);
    }
    SDL_SetGPUScissor(pass, &full);
    SDL_GPUBufferBinding qb{qbuf_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &qb, 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);

    shadow_frame_.width = pixel_w;
    shadow_frame_.height = pixel_h;
    shadow_frame_.to_screen[0] = float(w) / float(pixel_w);
    shadow_frame_.to_screen[1] = float(h) / float(pixel_h);
    if (main_camera_ >= cameras_.size()) return true;
    const auto camera = cameras_.begin() + main_camera_;
    shadow_frame_.origin[0] = camera->first[0];
    shadow_frame_.origin[1] = camera->first[1];
    shadow_frame_.focus[0] = camera->first[2];
    shadow_frame_.focus[1] = camera->first[3];
    const auto car = car_camera_ < cameras_.size() ? cameras_.begin() + car_camera_ : camera;
    shadow_frame_.origin2[0] = car->first[0];
    shadow_frame_.origin2[1] = car->first[1];
    shadow_frame_.focus2[0] = car->first[2];
    shadow_frame_.focus2[1] = car->first[3];
    shadow_frame_.cutout_xyz = &cutout_xyz_;
    shadow_frame_.cutout_records = &cutout_records_;
    shadow_frame_.tex0 = have_mem ? mem.tex0 : nullptr;
    shadow_frame_.tex1 = have_mem ? mem.tex1 : nullptr;
    shadow_frame_.tex_generation = mem.tex_generation;
    frame_w_ = w, frame_h_ = h;
    shadow_ready_ = true;
    return true;
}

bool GpuRenderer::enable_shadows() {
    if (shadows_) return true;
    if (!ok() || !(SDL_GetGPUShaderFormats(dev_) & SDL_GPU_SHADERFORMAT_DXIL)) return false;
    auto dxil = [&](const unsigned char *code, size_t size, const char *entry, SDL_GPUShaderStage stage,
                    int uniforms, int samplers, int storage_buffers) {
        SDL_GPUShaderCreateInfo si{};
        si.stage = stage;
        si.format = SDL_GPU_SHADERFORMAT_DXIL;
        si.code = code;
        si.code_size = size;
        si.entrypoint = entry;
        si.num_uniform_buffers = Uint32(uniforms);
        si.num_samplers = Uint32(samplers);
        si.num_storage_buffers = Uint32(storage_buffers);
        return SDL_CreateGPUShader(dev_, &si);
    };
    SDL_GPUShader *vs_poly = dxil(k_vs_poly_dxil, sizeof k_vs_poly_dxil, "vs_poly", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0, 0);
    SDL_GPUShader *vs_quad = dxil(k_vs_quad_dxil, sizeof k_vs_quad_dxil, "vs_quad", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0, 0);
    SDL_GPUShader *ps_z = dxil(k_ps_view_z_dxil, sizeof k_ps_view_z_dxil, "ps_view_z", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 0);
    SDL_GPUShader *ps_z2 = dxil(k_ps_view_z2_dxil, sizeof k_ps_view_z2_dxil, "ps_view_z2", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 0);
    SDL_GPUShader *ps_front = dxil(k_ps_front_z_dxil, sizeof k_ps_front_z_dxil, "ps_front_z",
                                   SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 2);
    SDL_GPUShader *ps_quad = dxil(k_ps_quad_z_dxil, sizeof k_ps_quad_z_dxil, "ps_quad_z", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 0);
    SDL_GPUShader *ps_shade = dxil(k_ps_shade_dxil, sizeof k_ps_shade_dxil, "ps_shade", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 0);
    const bool shaders = vs_poly && vs_quad && ps_z && ps_front && ps_quad && ps_shade;

    SDL_GPUColorTargetDescription z_color{};
    z_color.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUVertexBufferDescription pvb{0, sizeof(PolyVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute pattr[6] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(PolyVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, depth))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, Uint32(offsetof(PolyVertex, ooz))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, Uint32(offsetof(PolyVertex, r))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, Uint32(offsetof(PolyVertex, info))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT2, Uint32(offsetof(PolyVertex, more))},
    };
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs_poly;
    pi.fragment_shader = ps_z;
    pi.vertex_input_state.vertex_buffer_descriptions = &pvb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = pattr;
    pi.vertex_input_state.num_vertex_attributes = 6;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = false;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_EQUAL; // only the polygon that won the pixel
    pi.target_info.color_target_descriptions = &z_color;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    pi.target_info.has_depth_stencil_target = true;
    if (shaders) z_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    if (shaders && ps_z2) { // optional: without it the car simply gets no shadow
        pi.fragment_shader = ps_z2;
        car_z_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    }

    SDL_GPUVertexBufferDescription qvb{0, sizeof(QuadVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute qattr[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, u))},
    };
    SDL_GPUGraphicsPipelineCreateInfo qi{};
    qi.vertex_shader = vs_quad;
    qi.vertex_input_state.vertex_buffer_descriptions = &qvb;
    qi.vertex_input_state.num_vertex_buffers = 1;
    qi.vertex_input_state.vertex_attributes = qattr;
    qi.vertex_input_state.num_vertex_attributes = 2;
    qi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    qi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    qi.target_info.color_target_descriptions = &z_color;
    qi.target_info.num_color_targets = 1;
    qi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    qi.target_info.has_depth_stencil_target = true; // same pass as the z of the polygons
    if (shaders) {
        qi.fragment_shader = ps_front;
        z_front_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
        qi.fragment_shader = ps_quad;
        z_quad_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    }
    // the frame times the light, plus the light shafts: src.rgb + dst * src.a
    // (dxr.hlsl ps_shade), its own pass (no depth); without shafts src.rgb
    // is 0 and this is dst * light as before
    SDL_GPUColorTargetDescription shade{};
    shade.format = frame_format_;
    shade.blend_state.enable_blend = true;
    shade.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    shade.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    shade.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    shade.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    shade.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    shade.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    qi.target_info.color_target_descriptions = &shade;
    qi.target_info.has_depth_stencil_target = false;
    qi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    if (shaders) {
        qi.fragment_shader = ps_shade;
        shade_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);
    }
    for (SDL_GPUShader *s : {vs_poly, vs_quad, ps_z, ps_z2, ps_front, ps_quad, ps_shade})
        if (s) SDL_ReleaseGPUShader(dev_, s);
    if (!z_pipe_ || !z_front_pipe_ || !z_quad_pipe_ || !shade_pipe_) {
        error_ = std::string("shadow pipelines: ") + SDL_GetError();
        for (SDL_GPUGraphicsPipeline **p : {&z_pipe_, &z_front_pipe_, &z_quad_pipe_, &shade_pipe_, &car_z_pipe_})
            if (*p) SDL_ReleaseGPUGraphicsPipeline(dev_, *p), *p = nullptr;
        return false;
    }
    shadows_ = true;
    return true;
}

// The view z and light textures, the target's size; their D3D12 resources
// come back through the creation properties (patch 0002).
bool GpuRenderer::ensure_shadow_targets(int pixel_w, int pixel_h) {
    if (view_z_ && pixel_w == shadow_frame_.width && pixel_h == shadow_frame_.height && shadow_frame_.view_z &&
        shadow_frame_.light)
        return true;
    if (view_z_) SDL_ReleaseGPUTexture(dev_, view_z_), view_z_ = nullptr;
    if (light_) SDL_ReleaseGPUTexture(dev_, light_), light_ = nullptr;
    shadow_frame_ = ShadowFrame{};
    auto make = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, void *&resource) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = format;
        ti.usage = usage;
        ti.width = Uint32(pixel_w);
        ti.height = Uint32(pixel_h);
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.props = SDL_CreateProperties();
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(dev_, &ti);
        resource = SDL_GetPointerProperty(ti.props, "SDL.gpu.texture.d3d12.resource", nullptr);
        SDL_DestroyProperties(ti.props);
        return texture;
    };
    // both stay in SDL's default state for a sampled texture (all shader resource)
    view_z_ = make(SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                   shadow_frame_.view_z);
    light_ = make(SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, // light, light shafts
                  SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE, shadow_frame_.light);
    shadow_frame_.width = pixel_w;
    shadow_frame_.height = pixel_h;
    return view_z_ && light_ && shadow_frame_.view_z && shadow_frame_.light;
}

// dxr.hlsl step 3: the target times the light the DXR pass wrote.
bool GpuRenderer::apply_shadow(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target) {
    if (!shadows_ || !light_ || !frame_w_) return false;
    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    if (!pass) return false;
    const SDL_GPUViewport viewport{0, 0, float(shadow_frame_.width), float(shadow_frame_.height), 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect full{0, 0, shadow_frame_.width, shadow_frame_.height};
    SDL_SetGPUScissor(pass, &full);
    const float screen[4] = {float(frame_w_), float(frame_h_), 0, 0};
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    SDL_BindGPUGraphicsPipeline(pass, shade_pipe_);
    SDL_GPUBufferBinding qb{qbuf_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &qb, 1);
    SDL_GPUTextureSamplerBinding tsb{light_, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

} // namespace app
