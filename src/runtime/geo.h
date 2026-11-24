// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// Model 2 geometrizer and the rasterizer's front end (culling, clipping,
// z-sort keys): the display list, as native C++. The original Model 2's
// geometrizer runs code from ROM inside its DSP that nobody has dumped, so
// this is MAME's high-level implementation (src/mame/sega/model2_v.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81, BSD-3-Clause; notice above kept
// as the licence requires), transplanted: geo_parse and its commands,
// model2_3d_push, model2_3d_process_polygon. Memory is read through
// bounds-checked cursors, so a display list that runs off its buffer is a
// hard error instead of MAME's undefined behaviour. See THIRD_PARTY.md.
#pragma once

#include "runtime/overlay_role_probe.h"
#include "runtime/fast_paths.h"
#include "runtime/mobile_identity.h"
#include "runtime/background_motion.h"
#include "runtime/snapshot_timing.h"
#include "runtime/rom_source.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace rt {

// Optional, env-gated object trace for the native-scene feasibility test.
// M2_OBJECT_TRACE=<path.csv>; no effect when unset.
namespace m2trace {
void frame_begin(uint64_t frame);
void frame_end();
void begin_object(uint32_t oba, uint32_t obc, uint32_t mode, const float *matrix,
                  uint32_t tpa = 0, uint32_t tha = 0);
void begin_direct(uint32_t tpa = 0, uint32_t tha = 0);
void count_vertices(uint32_t n);
void finish();
}


struct GeoFatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct GeoVertex {
    float x = 0, y = 0;
    float p[3] = {0, 0, 0}; // pz, pu, pv
    float ox = 0, oy = 0, oz = 0; // object-space position before the matrix
};

// A ROM object as submitted, before the rasterizer drops or projects faces.
// Keep the raw link attributes (including lighting and distance selectors).
// Texture coordinates and headers belong to this emission: the same ROM
// geometry can be emitted with different pointers or mutable texture RAM.
struct RomObjectPrimitive {
    struct Vertex {
        std::array<float, 3> position{};
        uint16_t u = 0, v = 0;
    };
    std::array<Vertex, 4> vertices{};
    std::array<float, 3> normal{};
    uint32_t attributes = 0;
    uint32_t tpa = 0, tha = 0;
    std::array<uint16_t, 4> texture_header{};
    uint8_t vertex_count = 0;
    bool has_rom_normal = false;
};

struct RomObjectEmission {
    uint32_t oba = 0, obc = 0, opcode = 0, command = 0, command_word = 0;
    uint32_t tpa = 0, tha = 0;
    uint8_t mode = 0;
    std::array<float, 12> matrix{};
    float focus_x = 0, focus_y = 0;
    std::vector<RomObjectPrimitive> primitives;
};

// One polygon kept for rendering (after culling and clipping), as MAME's
// model2_3d_process_polygon adds it to its list.
struct GeoPoly {
    uint16_t z = 0;
    int32_t object_z_adjust = 0;
    uint8_t local_texture = 0; // GPU-only local body surface; 0 uses the board texture state
    uint16_t local_solid_palette = 0; // authored RGB555 for a solid face; 0 uses palette RAM
    uint16_t texheader[4] = {0, 0, 0, 0};
    uint8_t luma = 0;
    int32_t texlod = 0;
    int16_t viewport[4] = {0, 0, 0, 0};
    int16_t center[2] = {0, 0};
    uint8_t window = 0;
    uint32_t reverse = 0;
    uint32_t object_oba = 0;
    uint32_t object_obc = 0;
    uint32_t object_command = 0; // 1-based object emission in this frame; not a temporal ID
    MobileKey mobile_key{};
    uint8_t object_mode = 0;
    uint8_t object_direct = 0;
    float object_matrix[12] = {};
    float object_focus_x = 1.0f, object_focus_y = 1.0f;
    uint8_t source_num_vertices = 0;
    float source_v[8][5] = {}; // pre-clip object position + UV, for the mesh cache
    uint8_t num_vertices = 0;
#ifdef M2_FAST_GEO
    // Not zero-filled for every polygon kept (160 bytes): only
    // v[0..num_vertices) is ever read, and model2_3d_process_polygon writes
    // those.
    GeoPoly() {}
    union { GeoVertex v[8]; };
#else
    GeoVertex v[8];
#endif
};

namespace m2trace {
void dump_mesh_frame(const std::vector<GeoPoly> &polys, uint64_t frame);
}

// Cursors standing in for MAME's raw u32/u16 pointers. Reads run on from a
// masked start, so a start near the end runs off it (time attack does, in
// polygon RAM and in texture memory). MAME reads past its array there; we
// wrap, as the hardware's address counter would. Sizes are powers of 2.
struct GeoPtr {
    uint32_t *base = nullptr;
    uint32_t size = 0, i = 0;
#ifdef M2_DC_MEMORY
    RomSource *rom = nullptr; // the polygon ROM, read through pages (base is null)
    mutable uint32_t rom_word = 0;
#endif
    bool null() const { return base == nullptr; }
    uint32_t &at() const {
#ifdef M2_DC_MEMORY
        if (rom) return rom_word = rom->dword(RomRegion::Polygons, (i & (size - 1)) * 4);
#endif
        if (!base) throw GeoFatal("geometrizer read from missing memory");
        return base[i & (size - 1)];
    }
    uint32_t &operator*() const { return at(); }
    GeoPtr operator++(int) { GeoPtr t = *this; ++i; return t; }
    GeoPtr &operator+=(uint32_t n) { i += n; return *this; }
#ifdef M2_FAST_GEO
    // n words from here into out, moving past them: the values n times
    // *(*this)++ gives, a page (or up to the wrap) at a time instead of a
    // page lookup each. (The ROM's pages are little-endian words, as the
    // Dreamcast's M2_DC_MEMORY supplies.)
    void read(uint32_t *out, uint32_t n) {
        while (n) {
            const uint32_t w = i & (size - 1);
            uint32_t run = n < size - w ? n : size - w;
            const uint32_t *src;
#ifdef M2_DC_MEMORY
            if (rom) {
                constexpr uint32_t kWords = RomSource::kPageSize / 4;
                if (run > kWords - (w & (kWords - 1))) run = kWords - (w & (kWords - 1));
                src = reinterpret_cast<const uint32_t *>(rom->page_fast(RomRegion::Polygons, w / kWords)) +
                      (w & (kWords - 1));
            } else
#endif
            {
                if (!base) throw GeoFatal("geometrizer read from missing memory");
                src = base + w;
            }
            for (uint32_t k = 0; k < run; ++k) out[k] = src[k];
            out += run;
            n -= run;
            i += run;
        }
    }
#endif
};
struct GeoPtr16 {
    const uint16_t *base = nullptr;
    uint32_t size = 0, i = 0;
#ifdef M2_DC_MEMORY
    RomSource *rom = nullptr; // the texture ROM, read through pages (base is null)
#endif
    uint16_t operator*() const {
#ifdef M2_DC_MEMORY
        if (rom) return rom->word(RomRegion::Textures, (i & (size - 1)) * 2);
#endif
        if (!base) throw GeoFatal("rasterizer read from missing texture memory");
        return base[i & (size - 1)];
    }
    GeoPtr16 operator++(int) { GeoPtr16 t = *this; ++i; return t; }
#ifdef M2_FAST_GEO
    // As GeoPtr::read, 16-bit words.
    void read(uint16_t *out, uint32_t n) {
        while (n) {
            const uint32_t w = i & (size - 1);
            uint32_t run = n < size - w ? n : size - w;
            const uint16_t *src;
#ifdef M2_DC_MEMORY
            if (rom) {
                constexpr uint32_t kWords = RomSource::kPageSize / 2;
                if (run > kWords - (w & (kWords - 1))) run = kWords - (w & (kWords - 1));
                src = reinterpret_cast<const uint16_t *>(rom->page_fast(RomRegion::Textures, w / kWords)) +
                      (w & (kWords - 1));
            } else
#endif
            {
                if (!base) throw GeoFatal("rasterizer read from missing texture memory");
                src = base + w;
            }
            for (uint32_t k = 0; k < run; ++k) out[k] = src[k];
            out += run;
            n -= run;
            i += run;
        }
    }
#endif
};

class BufferWriteProvenance;
class CameraLineage;

class Geo {
public:
    // polygons: the model ROM (0x1000000 bytes); textures: the texture ROM
    // (0x1000000 bytes); buffer: buffer RAM (0x8000 dwords), which the
    // display list is read from.
    Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer);
#ifdef M2_DC_MEMORY
    // Both ROMs read through pages, not copied.
    Geo(RomSource &rom, uint32_t *buffer);
#endif

    // MAME screen_vblank -> geo_parse: walk this frame's display list from
    // read_start (the geometrizer's read-start register).
    void parse(uint32_t read_start);
    // Opt-in reference path: rerasterize the ordered geometry emissions.
    // Used for alpha=1 validation before the pose interpolation work.
    void enable_instance_reference(bool recalculate_facing = false) {
        instance_reference_ = true;
        recalculate_facing_ = recalculate_facing;
    }
    void set_write_provenance(const BufferWriteProvenance *p) { write_provenance_ = p; }
    OverlayRoleProbe &overlay_role_probe() { return overlay_roles_; }
    void set_camera_lineage(CameraLineage *value, const BufferWriteProvenance *provenance) {
        camera_lineage_ = value;
        camera_write_provenance_ = provenance;
    }
    // The emission is valid during the callback; command is local to the draw.
    using RomObjectSink = void (*)(void *, const RomObjectEmission &);
    void set_rom_object_sink(RomObjectSink sink, void *context) {
        rom_object_sink_ = sink;
        rom_object_context_ = context;
    }
    void set_mobile_identity(const MobileIdentity *keys) { mobile_identity_ = keys; }
    void set_tick(uint64_t tick) { tick_ = tick; }
    void set_mobile_key_trace(std::FILE *file) { mobile_key_trace_ = file; }

    void enable_pose_presentation() {
        if (track_shaft_route_ && !pose_presentation_) discard_pose_history();
        pose_presentation_ = recalculate_facing_ = true;
    }
    void stamp_pose_scene(SnapshotStamp stamp);
    void track_shaft_route(bool enable);
    double shaft_route(bool presented) const { return presented ? presented_shaft_route_ : shaft_route_; }
    const std::vector<GeoPoly> *present_pose_scene(uint64_t now_ns, uint64_t delay_ns);
    void discard_pose_history();

    // This frame's output.
    std::vector<GeoPoly> polys;       // kept polygons, in the order added
    std::vector<uint32_t> pushed;     // every word handed to the rasterizer
    // Current object state, for the optional mesh dump.
    uint32_t cur_oba = 0, cur_obc = 0;
    uint32_t cur_object_command = 0;
    uint8_t cur_mode = 0, cur_direct = 0;
    float cur_matrix[12] = {};
    float cur_focus_x = 1.0f, cur_focus_y = 1.0f;
    bool record_pushes = false;
#ifdef M2_DC_SPEED
    // Draw mode, a frame whose polygons will not be shown: object data is
    // not parsed (the rasterizer gets its opening and closing words only).
    // Every other command still runs, so the geometrizer's state is the same.
    bool skip_objects = false;
#endif

    struct plane {
        GeoVertex normal;
        float distance = 0;
    };
    struct texture_parameter {
        float diffuse = 0, ambient = 0;
        uint32_t specular_control = 0;
        float specular_scale = 0;
    };
    struct raster_state {
        const uint16_t *texture_rom = nullptr;
        uint32_t texture_rom_mask = 0;
#ifdef M2_DC_MEMORY
        RomSource *rom = nullptr;
#endif
        int16_t viewport[4] = {0, 0, 0, 0};
        int16_t center[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        uint16_t center_sel = 0;
        uint32_t reverse = 0;
        int32_t z_adjust = 0;
        float polygon_z = 0;
        uint8_t master_z_clip = 0;
        uint32_t cur_command = 0;
        uint32_t command_buffer[32] = {};
        float object_pos[32][3] = {}; // pre-matrix position for each point slot
        uint32_t command_index = 0;
        uint32_t poly_list_index = 0;
        uint16_t min_z = 0, max_z = 0;
        uint16_t texture_ram[0x10000] = {};
        uint8_t log_ram[0x8000] = {};
        uint8_t cur_window = 0;
        plane clip_plane[4][4];
    };
    struct geo_state {
        raster_state *raster = nullptr;
        uint32_t mode = 0;
        uint32_t *polygon_rom = nullptr;
        uint32_t polygon_rom_mask = 0;
#ifdef M2_DC_MEMORY
        RomSource *rom = nullptr;
#endif
        float matrix[12] = {};
        GeoVertex focus, light;
        float lod = 0;
        float coef_table[32] = {};
        texture_parameter texture_parameters[32];
        uint32_t polygon_ram0[0x8000] = {};
        uint32_t polygon_ram1[0x8000] = {};
    };

    static uint16_t z_sort_value(float depth, int32_t z_adjust);
    int windows() const { return raster_.cur_window; } // after parse: the last window used
    // The light vector (command 0A), camera space: faces are lit by
    // dot(normal, light). Read by the ray-traced shadows (app/gpu/dxr_scene.h).
    std::array<float, 3> light() const { return {geo_.light.x, geo_.light.y, geo_.light.p[0]}; }
    // The Revision A HUD groups drawn as tiny objects just in front of the eye
    // (minimap, rev needle...): the addresses is_camera_space_overlay checks
    // (geo.cpp). Not part of the world: they cast and receive no shadow.
    static bool hud_overlay_address(uint32_t address) {
        switch (address & 0x1ffffffu) {
        case 0x8d5da4u: case 0x8d5efau: case 0x8d5f4bu: case 0x8d5f9cu: case 0x8d60cdu:
        case 0x8d61bfu: case 0x8d65eeu: case 0x8d6a0fu: case 0x94dd59u: return true;
        default: return false;
        }
    }
    void zclip_w(uint32_t data) { raster_.master_z_clip = uint8_t(data); } // model2_3d_zclip_w
    // Widescreen (enhancement, 0 = off): a full-width viewport's left and right
    // clip planes move out by this many pixels, so polygons beyond the 4:3
    // edges survive for the wider screen. Applies from the next window command.
    void set_wide_margin(int pixels) { wide_margin_ = pixels; }
    // Ray-traced shadows: full-screen viewports keep this many more pixels
    // above their top edge and beside their sides (0 = off, the game's own clipping).
    void set_top_margin(int pixels) { top_margin_ = pixels; }
    // Ray-traced shadows (on with the top margin): the polygons wholly behind
    // the camera, never drawn, only cast (the sun behind, a tree just passed
    // still shades the road ahead: 23/11). The game's frame's, or the
    // last presented (interpolated) scene's.
    const std::vector<GeoPoly> &shadow_casters(bool presented) const {
        return presented ? presented_casters_ : shadow_casters_;
    }

private:
    struct ReferencePrimitive {
        RomObjectPrimitive geometry;
        uint32_t attributes = 0;
        std::array<float, 12> matrix{};
        std::array<GeoVertex, 4> fallback_vertices{};
        float focus_x = 0, focus_y = 0;
        std::array<plane, 4> planes{};
        int16_t viewport[4] = {}, center[2] = {};
        uint8_t window = 0, master_z_clip = 0, luma = 0;
        uint32_t reverse = 0, face_word = 0;
        int32_t z_adjust = 0, texlod = 0;
        uint32_t oba = 0, obc = 0, command = 0;
        uint8_t mode = 0, direct = 0;
        bool rom = false;
        uint32_t reflection_pose_command = 0;
        MobileKey mobile_key;
    };
    struct SceneObject {
        BackgroundObject geometry;
        MobileKey key;
    };
    std::vector<SceneObject> current_scene_, previous_scene_;
    std::vector<GeoPoly> presented_polys_;
    std::vector<GeoPoly> shadow_casters_, presented_casters_; // shadow_casters()
    std::vector<GeoPoly> *replay_casters_ = nullptr;          // filled by replay_instance_reference
    std::vector<std::array<float, 12>> sampled_matrices_;
    std::vector<uint8_t> pose_branch_; // present_pose_scene: how each object's pose was chosen
    std::optional<SnapshotStamp> previous_stamp_, current_stamp_;
    std::optional<BackgroundMotion> background_motion_;
    // The pair of ticks before the current one (n-2, n-1), kept whole so a
    // presentation sampled before tick n-1 is interpolated there instead of
    // being held at alpha 0: the third snapshot.
    struct PosePair {
        std::vector<SceneObject> previous_scene, current_scene;
        std::vector<ReferencePrimitive> primitives;
        SnapshotStamp previous_stamp, current_stamp;
        BackgroundMotion motion;
        // The tick before previous_stamp and the motion from it to
        // previous_stamp: the speed at this pair's first tick (Hermite steps).
        std::vector<SceneObject> before_scene;
        std::array<double, 12> before_motion{};
        bool before_valid = false;
    };
    std::optional<PosePair> older_pair_;
    bool presenting_older_ = false; // M2_POSE_TRACE writes its alpha minus 1
    void swap_pose_pair(PosePair &pair);
    void rotate_pose_frame(); // a new tick: the current pair becomes the older one
    const std::vector<GeoPoly> *present_pose_pair(uint64_t now_ns, uint64_t delay_ns);
    bool pose_presentation_ = false;

    bool instance_reference_ = false;
    bool recalculate_facing_ = false;
    std::vector<ReferencePrimitive> reference_primitives_;
    RomObjectSink rom_object_sink_ = nullptr;
    void *rom_object_context_ = nullptr;
    bool capturing_rom_object_ = false;
    uint32_t command_word_ = 0;
    uint32_t rom_link_attributes_ = 0;
    std::array<float, 3> rom_link_normal_{};
    RomObjectEmission rom_object_;

    const MobileIdentity *mobile_identity_ = nullptr;
    std::FILE *mobile_key_trace_ = nullptr;
    uint64_t tick_ = 0;
    MobileKey current_mobile_key_{};

    const BufferWriteProvenance *write_provenance_ = nullptr;
    CameraLineage *camera_lineage_ = nullptr;
    const BufferWriteProvenance *camera_write_provenance_ = nullptr;
    int wide_margin_ = 0;
    int top_margin_ = 0;
    std::vector<uint32_t> polygon_rom_;
    std::vector<uint16_t> texture_rom_;
    uint32_t *buffer_;
    raster_state raster_;
    geo_state geo_;

    GeoPtr buf(uint32_t i) { return GeoPtr{buffer_, 0x8000, i}; }
    bool check_culling(raster_state *raster, uint32_t attr, float min_z, float max_z);
    template <unsigned NumVerts> void model2_3d_process_polygon(raster_state *raster, uint32_t attr);
    template <unsigned NumVerts> void capture_rom_primitive(const raster_state &raster,
                                                            GeoPtr16 tp, GeoPtr16 th);
    void bind_windshield_reflection();
    void bind_ram_emissions();
    void replay_instance_reference(std::vector<GeoPoly> &output,
                                   bool recalculate_facing,
                                   std::span<const std::array<float, 12>> poses = {});
    void replay_instance_reference() { replay_instance_reference(polys, recalculate_facing_); }
    void model2_3d_push(raster_state *raster, uint32_t input);
    void model2_3d_push_point(raster_state *raster, const GeoVertex &point);
    void render_frame_start();
    void geo_parse_np_ns(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_np_s(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_nn_ns(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_nn_s(geo_state *geo, GeoPtr input, uint32_t count);
    GeoPtr geo_nop(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_object_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_direct_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_window_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_texture_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_polygon_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_texture_parameters(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_mode(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_zsort_mode(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_focal_distance(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_light_source(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_matrix_write(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_translate_write(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_data_mem_push(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_test(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_end(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_dummy(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_log_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_lod(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_code_upload(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_code_jump(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_process_command(geo_state *geo, uint32_t opcode, GeoPtr input, bool *end_code);
    OverlayRoleProbe overlay_roles_;
    bool track_shaft_route_ = false;
    // The samples follow the three snapshot ticks, including the older pair.
    std::array<double, 3> shaft_route_ticks_{};
    double shaft_route_ = 0.0, presented_shaft_route_ = 0.0;
};

} // namespace rt
