#include "runtime/camera_lineage.h"
// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// Model 2 geometrizer (HLE) and rasterizer front end, transplanted from
// MAME's src/mame/sega/model2_v.cpp at dddd73680656e355bb2b5beecab1167c9f07bf81
// (BSD-3-Clause; notice above kept as the licence requires). The code below
// is MAME's with these changes only: raw pointers are bounds-checked cursors
// (GeoPtr, GeoPtr16); kept polygons go to a flat list (Geo::polys) instead of
// MAME's per-z buckets; logerror is dropped and fatalerror throws. See
// src/runtime/geo.h and THIRD_PARTY.md.

#include "runtime/geo.h"
#include "runtime/pose_interp.h"
#include "runtime/write_provenance.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "compat/bit_cast.h"
namespace rt {

namespace m2trace {
namespace {
std::FILE *trace_file = nullptr;
std::FILE *writer_file = nullptr;
bool trace_ready = false;
bool trace_active = false;
bool trace_direct = false;
uint64_t trace_frame = 0;
uint32_t trace_oba = 0, trace_obc = 0, trace_mode = 0, trace_vertices = 0;
float trace_matrix[12] = {};
// Command-buffer word offsets are provenance only, never persistent IDs.
uint32_t current_command_word = UINT32_MAX, current_opcode = 0;
uint32_t trace_command_word = UINT32_MAX, trace_opcode = 0;
uint32_t trace_tpa = 0, trace_tha = 0;

void ensure_file() {
    if (trace_ready) return;
    trace_ready = true;
    const char *path = std::getenv("M2_OBJECT_TRACE");
    if (path && *path) {
        trace_file = std::fopen(path, "wb");
        if (trace_file) {
            std::fprintf(trace_file,
                "frame,pool,oba,obc,mode,matrix0,matrix1,matrix2,matrix3,matrix4,"
                "matrix5,matrix6,matrix7,matrix8,matrix9,matrix10,matrix11,vertices,source,command_word,opcode,tpa,tha\n");
            std::fflush(trace_file);
        }
    }
}

void flush_object() {
    if (!trace_active || !trace_file) return;
    const char *source = trace_direct ? "direct"
                       : (trace_oba & 0x01000000) ? "RAM1"
                       : (trace_oba & 0x00800000) ? "ROM" : "RAM0";
    std::fprintf(trace_file, "%llu,0,%u,%u,%u", (unsigned long long)trace_frame,
                 trace_oba, trace_obc, trace_mode);
    for (float v : trace_matrix) std::fprintf(trace_file, ",%.9g", v);
    std::fprintf(trace_file, ",%u,%s,%u,%u,%u,%u\n", trace_vertices, source,
                 trace_command_word, trace_opcode, trace_tpa, trace_tha);
    std::fflush(trace_file);
    trace_active = false;
}
} // namespace

void frame_begin(uint64_t frame) {
    ensure_file();
    flush_object();
    trace_frame = frame;
    current_command_word = UINT32_MAX;
    current_opcode = 0;
}

void frame_end() {
    flush_object();
    if (writer_file && std::fflush(writer_file)) throw GeoFatal("writer trace flush failed");
}

// Snapshot only the command words actually used by object/direct commands.
// Per-bit masks describe last writers, not objects or writer instruction PCs.
void command_writers(uint32_t word, uint32_t opcode, const BufferWriteProvenance *provenance) {
    if (!provenance) return;
    const uint32_t kind = (opcode >> 23) & 31;
    const unsigned count = (kind == 1 || kind == 17) ? 5 : (kind == 2 || kind == 18) ? 3 : 0;
    if (!count) return;
    if (!writer_file) {
        const char *path = std::getenv("M2_WRITE_TRACE");
        if (!path || !*path) throw GeoFatal("writer trace path disappeared");
        writer_file = std::fopen(path, "wb");
        if (!writer_file) throw GeoFatal("cannot open writer trace");
        if (std::fprintf(writer_file, "frame,command_word,argument_index,buffer_word,unknown_mask,i960_mask,tgp_mask,port_mask,last_mask,writes\n") < 0)
            throw GeoFatal("writer trace header failed");
    }
    for (unsigned argument = 0; argument < count; ++argument) {
        const auto index = (word + argument) & 0x7fff;
        const auto &p = provenance->at(index);
        if (std::fprintf(writer_file, "%llu,%u,%u,%u,%u,%u,%u,%u,%u,%llu\n",
                (unsigned long long)trace_frame, word, argument, index,
                p.unknown, p.i960, p.tgp, p.geometry_port, p.last_mask,
                (unsigned long long)p.writes) < 0)
            throw GeoFatal("writer trace row failed");
    }
}

void command_begin(uint32_t word, uint32_t opcode) {
    current_command_word = word;
    current_opcode = opcode;
}

void begin_object(uint32_t oba, uint32_t obc, uint32_t mode, const float *matrix,
                  uint32_t tpa, uint32_t tha) {
    ensure_file();
    flush_object();
    trace_oba = oba; trace_obc = obc; trace_mode = mode;
    trace_direct = false;
    trace_command_word = current_command_word; trace_opcode = current_opcode;
    trace_tpa = tpa; trace_tha = tha;
    for (unsigned i = 0; i < 12; ++i) trace_matrix[i] = matrix[i];
    trace_vertices = 0;
    trace_active = true;
}

void begin_direct(uint32_t tpa, uint32_t tha) {
    static const float zero[12] = {};
    begin_object(0, 0, 0, zero, tpa, tha);
    trace_direct = true;
}

void count_vertices(uint32_t n) {
    if (trace_active) trace_vertices += n;
}

void finish() {
    flush_object();
    if (trace_file) { std::fclose(trace_file); trace_file = nullptr; }
    if (writer_file) { std::fclose(writer_file); writer_file = nullptr; }
}

void dump_mesh_frame(const std::vector<GeoPoly> &polys, uint64_t frame) {
    const char *path = std::getenv("M2_MESH_DUMP");
    if (!path || !*path) return;
    const char *frame_env = std::getenv("M2_MESH_FRAME");
    const uint64_t want = frame_env ? std::strtoull(frame_env, nullptr, 10) : 6000;
    if (frame != want) return;
    static bool done = false;
    if (done) return;
    done = true;

    std::FILE *out = std::fopen(path, "wb");
    if (!out) return;
    std::fprintf(out, "frame,poly,oba,obc,mode,direct,fx,fy,m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,texheader0,texheader1,texheader2,texheader3,luma,u,v,sx,sy,sz,ox,oy,oz\n");
    for (size_t p = 0; p < polys.size(); ++p) {
        const GeoPoly &poly = polys[p];
        for (unsigned k = 0; k < poly.num_vertices; ++k) {
            const GeoVertex &v = poly.v[k];
            const double ox = v.ox, oy = v.oy, oz = v.oz;
            std::fprintf(out, "%llu,%zu,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                         (unsigned long long)frame, p, poly.object_oba, poly.object_obc,
                         poly.object_mode, poly.object_direct,
                         poly.object_focus_x, poly.object_focus_y,
                         poly.object_matrix[0], poly.object_matrix[1], poly.object_matrix[2],
                         poly.object_matrix[3], poly.object_matrix[4], poly.object_matrix[5],
                         poly.object_matrix[6], poly.object_matrix[7], poly.object_matrix[8],
                         poly.object_matrix[9], poly.object_matrix[10], poly.object_matrix[11],
                         poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.texheader[3],
                         poly.luma, double(v.p[1]), double(v.p[2]), double(v.x), double(v.y),
                         double(v.p[0]), ox, oy, oz);
        }
    }
    std::fclose(out);
}

} // namespace m2trace


namespace {
inline float u2f(uint32_t v) { return compat::bit_cast<float>(v); }
inline uint32_t f2u(float f) { return compat::bit_cast<uint32_t>(f); }
using u8 = uint8_t;
using u64 = uint64_t;
} // namespace

#define pz      p[0]
#define pu      p[1]
#define pv      p[2]

struct quad_m2 {
    GeoVertex v[4];
    uint16_t z = 0;
    uint16_t texheader[4] = {0, 0, 0, 0};
    uint8_t luma = 0;
    int32_t texlod = 0;
};

Geo::Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer)
    : polygon_rom_(polygons.size() / 4), texture_rom_(textures.size() / 2), buffer_(buffer) {
    if (polygon_rom_.empty() || (polygon_rom_.size() & (polygon_rom_.size() - 1)) || texture_rom_.empty() ||
        (texture_rom_.size() & (texture_rom_.size() - 1)))
        throw GeoFatal("bad polygon or texture ROM image");
    std::memcpy(polygon_rom_.data(), polygons.data(), polygon_rom_.size() * 4);
    std::memcpy(texture_rom_.data(), textures.data(), texture_rom_.size() * 2);
    raster_.texture_rom = texture_rom_.data();
    raster_.texture_rom_mask = uint32_t(texture_rom_.size() - 1);
    geo_.raster = &raster_;
    geo_.polygon_rom = polygon_rom_.data();
    geo_.polygon_rom_mask = uint32_t(polygon_rom_.size() - 1);
}

#ifdef M2_DC_MEMORY
Geo::Geo(RomSource &rom, uint32_t *buffer) : buffer_(buffer) {
    const uint32_t polygon_words = rom.size(RomRegion::Polygons) / 4, texels = rom.size(RomRegion::Textures) / 2;
    if (!polygon_words || (polygon_words & (polygon_words - 1)) || !texels || (texels & (texels - 1)))
        throw GeoFatal("bad polygon or texture ROM size");
    raster_.rom = &rom;
    raster_.texture_rom_mask = texels - 1;
    geo_.raster = &raster_;
    geo_.rom = &rom;
    geo_.polygon_rom_mask = polygon_words - 1;
    // Room for the busiest frame up front (attract and race: 2,182 kept
    // polygons, ~200 bytes each): growing past 2,048 doubles the vector to
    // 800 KB while the old 400 KB still exists, more than 16 MB has spare.
    polys.reserve(2400);
}
#endif

static inline void transform_point(GeoVertex *point, const float *matrix)
{
	float tx = (point->x * matrix[0]) + (point->y * matrix[3]) + (point->pz * matrix[6]) + (matrix[9]);
	float ty = (point->x * matrix[1]) + (point->y * matrix[4]) + (point->pz * matrix[7]) + (matrix[10]);
	float tz = (point->x * matrix[2]) + (point->y * matrix[5]) + (point->pz * matrix[8]) + (matrix[11]);

	point->x = tx;
	point->y = ty;
	point->pz = tz;
}

static inline void transform_vector(GeoVertex *vector, const float *matrix)
{
	float tx = (vector->x * matrix[0]) + (vector->y * matrix[3]) + (vector->pz * matrix[6]);
	float ty = (vector->x * matrix[1]) + (vector->y * matrix[4]) + (vector->pz * matrix[7]);
	float tz = (vector->x * matrix[2]) + (vector->y * matrix[5]) + (vector->pz * matrix[8]);

	vector->x = tx;
	vector->y = ty;
	vector->pz = tz;
}

#if defined(M2_DC_NATIVE_GEO) && defined(__sh__)
// The Dreamcast (picture only: nothing the geometrizer computes goes back
// into the game): the SH-4 transforms a vector by its 4x4 matrix registers
// (XMTRX) in one ftrv. The 3x4 matrix is loaded once per object as
// columns (m0 m1 m2 0) (m3 m4 m5 0) (m6 m7 m8 0) (m9 m10 m11 1); a point
// has w 1, a vector w 0 (no translation), as transform_point/_vector.
// ftrv's rounding is not the separate multiplies' and adds': the polygons
// differ from the desktop's in their last bits.
static inline void xmtrx_load(const float *matrix)
{
	alignas(8) float m[16] = {matrix[0], matrix[1], matrix[2], 0.0f, matrix[3], matrix[4], matrix[5], 0.0f,
	                          matrix[6], matrix[7], matrix[8], 0.0f, matrix[9], matrix[10], matrix[11], 1.0f};
	const float *p = m;
	__asm__ __volatile__(
		"fschg\n\t"
		"fmov.d @%0+, xd0\n\t"
		"fmov.d @%0+, xd2\n\t"
		"fmov.d @%0+, xd4\n\t"
		"fmov.d @%0+, xd6\n\t"
		"fmov.d @%0+, xd8\n\t"
		"fmov.d @%0+, xd10\n\t"
		"fmov.d @%0+, xd12\n\t"
		"fmov.d @%0+, xd14\n\t"
		"fschg\n"
		: "+r"(p) : : "memory");
}
static inline void xmtrx_apply(GeoVertex *v, float w)
{
	register float x __asm__("fr0") = v->x;
	register float y __asm__("fr1") = v->y;
	register float z __asm__("fr2") = v->pz;
	register float ww __asm__("fr3") = w;
	__asm__ __volatile__("ftrv xmtrx, fv0\n" : "+f"(x), "+f"(y), "+f"(z), "+f"(ww));
	v->x = x;
	v->y = y;
	v->pz = z;
}
#define M2_GEO_LOAD_MATRIX(geo) xmtrx_load((geo)->matrix)
#define M2_GEO_POINT(p, geo) do { keep_object_position(p); xmtrx_apply((p), 1.0f); } while (0)
#define M2_GEO_VECTOR(v, geo) xmtrx_apply((v), 0.0f)
#else
#define M2_GEO_LOAD_MATRIX(geo) ((void)0)
#define M2_GEO_POINT(p, geo) do { keep_object_position(p); transform_point((p), (geo)->matrix); } while (0)
#define M2_GEO_VECTOR(v, geo) transform_vector((v), (geo)->matrix)
#endif
static inline void keep_object_position(GeoVertex *p)
{
	p->ox = p->x; p->oy = p->y; p->oz = p->pz;
}

// Three words (a point or a normal) from the input: one block read with
// M2_FAST_GEO (GeoPtr::read: the same words), otherwise word by word.
static inline void read_point(GeoPtr &input, GeoVertex &v)
{
#ifdef M2_FAST_GEO
	uint32_t w[3];
	input.read(w, 3);
	v.x = u2f(w[0]);
	v.y = u2f(w[1]);
	v.pz = u2f(w[2]);
#else
	v.x = u2f(*input++);
	v.y = u2f(*input++);
	v.pz = u2f(*input++);
#endif
}

static inline void normalize_vector(GeoVertex *vector)
{
	const float n = sqrt((vector->x * vector->x) + (vector->y * vector->y) + (vector->pz * vector->pz));

	if (n)
	{
		float oon = 1.0f / n;
		vector->x *= oon;
		vector->y *= oon;
		vector->pz *= oon;
	}
}

static inline float dot_product(const GeoVertex &v1, const GeoVertex &v2)
{
	return (v1.x * v2.x) + (v1.y * v2.y) + (v1.pz * v2.pz);
}

static inline void vector_cross3(GeoVertex *dst, const GeoVertex *v0, const GeoVertex *v1, const GeoVertex *v2)
{
	GeoVertex p1, p2;

	p1.x = v1->x - v0->x;   p1.y = v1->y - v0->y;   p1.pz = v1->pz - v0->pz;
	p2.x = v2->x - v0->x;   p2.y = v2->y - v0->y;   p2.pz = v2->pz - v0->pz;

	dst->x = (p1.y * p2.pz) - (p1.pz * p2.y);
	dst->y = (p1.pz * p2.x) - (p1.x * p2.pz);
	dst->pz = (p1.x * p2.y) - (p1.y * p2.x);
}

static inline void apply_focus(Geo::geo_state *geo, GeoVertex *p0)
{
	p0->x *= geo->focus.x;
	p0->y *= geo->focus.y;
}


/* 1.8.23 float to 4.12 float converter, courtesy of Aaron Giles */
uint16_t Geo::z_sort_value(float floatval, int32_t z_adjust)
{
	int32_t fpint = f2u(floatval);
	int32_t exponent = ((fpint >> 23) & 0xff) - ((z_adjust >> 23) & 0xff);
	uint32_t mantissa = fpint & 0x7fffff;

	/* round the low bits and reduce to 12 */
	mantissa += 0x400;
	if (mantissa > 0x7fffff)
	{
		exponent++;
		mantissa = (mantissa & 0x7fffff) >> 1;
	}
	mantissa >>= 11;

	// if negative, clamp to 0
	if (fpint < 0)
		return 0x0000;

	// the rest depends on the exponent
	if (exponent < -12)
		return 0x0000; // less than -12 is too small, return 0
	else if (exponent < 0)
		return (mantissa | 0x1000) >> -exponent; // between -12 and 0 create a denormal with exponent of 0
	else if (exponent < 15)
		return ((exponent + 1) << 12) | mantissa; // between 0 and 14 create a FP value with exponent + 1
	else
		return 0xffff; // above 14 is too large
}

static inline bool polygon_inside_plane(const GeoVertex *v, int32_t count, const Geo::plane &clip_plane)
{
    for (int32_t i = 0; i < count; ++i)
        if (!(dot_product(v[i], clip_plane.normal) >= clip_plane.distance))
            return false;
    return true;
}

static int32_t clip_polygon(GeoVertex *v, int32_t num_vertices, GeoVertex *vout, Geo::plane clip_plane)
{
	if (num_vertices <= 0)
		return 0;

	int32_t outcount = 0;

	const GeoVertex *cur = v;
	GeoVertex *out = vout;

	float curdot = dot_product(*cur, clip_plane.normal);
	int32_t curin = (curdot >= clip_plane.distance) ? 1 : 0;

	for (int32_t i = 0; i < num_vertices; i++)
	{
		// Cortex-A9 has no integer divide instruction. This wrap is exactly
		// equivalent to modulo for this bounded vertex loop.
		const int32_t nextvert = i + 1 == num_vertices ? 0 : i + 1;

		/* if the current point is inside the plane, add it */
		if (curin)
			out[outcount++] = *cur;

		const float nextdot = dot_product(v[nextvert], clip_plane.normal);
		const int32_t nextin = (nextdot >= clip_plane.distance) ? 1 : 0;

		/* Add a clipped vertex if one end of the current edge is inside the plane and the other is outside */
		if ((curin != nextin) && !std::isnan(curdot) && !std::isnan(nextdot))
		{
			const float scale = (clip_plane.distance - curdot) / (nextdot - curdot);

			out[outcount].x = cur->x + ((v[nextvert].x - cur->x) * scale);
			out[outcount].y = cur->y + ((v[nextvert].y - cur->y) * scale);
			out[outcount].pz = cur->pz + ((v[nextvert].pz - cur->pz) * scale);
			out[outcount].pu = cur->pu + ((v[nextvert].pu - cur->pu) * scale);
			out[outcount].pv = cur->pv + ((v[nextvert].pv - cur->pv) * scale);
			out[outcount].ox = cur->ox + ((v[nextvert].ox - cur->ox) * scale);
			out[outcount].oy = cur->oy + ((v[nextvert].oy - cur->oy) * scale);
			out[outcount].oz = cur->oz + ((v[nextvert].oz - cur->oz) * scale);
			outcount++;
		}

		curdot = nextdot;
		curin = nextin;
		cur++;
	}

	return outcount;
}

// Local play build: the game's master z clip (model2_3d_zclip_w, 0..254)
// drops every polygon farther than it, the board's distance limit. With
// M2_FAR_CLIP_OFF=1 it is ignored, so more distant scenery is drawn.
static bool far_clip_off() {
	static const bool off = [] {
		const char *v = std::getenv("M2_FAR_CLIP_OFF");
		return v && v[0] == '1';
	}();
	return off;
}

// Ray-traced shadows: the parts of a polygon outside the view, beyond each
// clip plane in turn (the view's own part is already in the frame). An
// exact partition: a face kept whole beside its own clipped part is the same
// surface triangulated twice, and a non-planar road quad then lies a hair
// above itself and shades the road in a band (Canyon checkpoint, 24/11).
template <class Emit>
static void outside_parts(const GeoVertex *whole, int count, const Geo::plane *planes, int num_planes, Emit emit)
{
	GeoVertex rest[2][8], piece[8];
	int n = count;
	std::copy_n(whole, count, rest[0]);
	int cur = 0;
	for (int p = 0; p < num_planes && n >= 3; ++p) {
		Geo::plane outside = planes[p];
		outside.normal.x = -outside.normal.x;
		outside.normal.y = -outside.normal.y;
		outside.normal.pz = -outside.normal.pz;
		outside.distance = -outside.distance;
		const int m = clip_polygon(rest[cur], n, piece, outside);
		if (m >= 3) emit(piece, m);
		n = clip_polygon(rest[cur], n, rest[cur ^ 1], planes[p]);
		cur ^= 1;
	}
}

inline bool Geo::check_culling(raster_state *raster, uint32_t attr, float min_z, float max_z)
{
	/* if doubleside is disabled */
	if (((attr >> 17) & 1) == 0)
	{
		/* if it's the backface, cull it */
		if (raster->command_buffer[9] & 0x00800000)
			return true;
	}

	/* if the linktype is 0, then we can also cull it */
	if (((attr >> 8) & 3) == 0)
		return true;

	/* if the minimum z value is bigger than the master z clip value, don't render */
	if (raster->master_z_clip != 0xff && !far_clip_off() && (int32_t)(1.0 / min_z) > raster->master_z_clip)
		return true;

	/* if the maximum z value is < 0 then we can safely clip the entire polygon */
	if (max_z < 0)
		return true;

	return false;
}

// Shadow casters behind the camera: how far back (camera units), and the
// switch (M2_DXR_BEHIND=0 drops them). 700 such polygons a tick on Beginner.
static constexpr float kCasterReach = 60.0f; // a high sun: shadows a third of a caster's height long
// Casters cut by the view's edges: how near the camera (camera units, z),
// M2_DXR_CUT_REACH (diagnostic; 0 drops them).
static float cut_reach() {
	static const float r = [] { const char *v = std::getenv("M2_DXR_CUT_REACH"); return v && *v ? float(std::atof(v)) : 60.0f; }();
	return r;
}
static bool behind_casters_on() {
	static const bool on = [] { const char *v = std::getenv("M2_DXR_BEHIND"); return !(v && v[0] == '0'); }();
	return on;
}

template <unsigned NumVerts>
void Geo::capture_rom_primitive(const raster_state &raster, GeoPtr16 tp, GeoPtr16 th)
{
    RomObjectPrimitive primitive;
    primitive.attributes = rom_link_attributes_;
    primitive.tpa = raster.command_buffer[0];
    primitive.tha = raster.command_buffer[1];
    primitive.vertex_count = NumVerts;
    primitive.normal = rom_link_normal_;
    primitive.has_rom_normal = rom_object_.mode < 2;
    constexpr unsigned slots[] = {5, 2, 11, 14};
    for (unsigned i = 0; i < NumVerts; ++i) {
        const auto &p = raster.object_pos[slots[i]];
        primitive.vertices[i].position = {p[0], p[1], p[2]};
        primitive.vertices[i].v = *tp++;
        primitive.vertices[i].u = *tp++;
    }
    if (!primitive.has_rom_normal) {
        const auto local = [](const RomObjectPrimitive::Vertex &v) {
            return GeoVertex{v.position[0], v.position[1], {v.position[2], 0, 0}};
        };
        const GeoVertex p0 = local(primitive.vertices[1]), p1 = local(primitive.vertices[0]);
        const GeoVertex p2 = local(primitive.vertices[2]);
        GeoVertex normal;
        vector_cross3(&normal, &p0, &p1, &p2);
        normalize_vector(&normal);
        primitive.normal = {normal.x, normal.y, normal.pz};
    }
    for (auto &word : primitive.texture_header) word = *th++;
    rom_object_.primitives.push_back(primitive);
}

// Diagnostic M2_GEO_SRC_TRACE (see the emitting path): one file shared by
// every template instance; returns it only for the wanted object and tick.
static std::FILE *geo_src_trace(uint64_t tick, uint32_t oba)
{
	static std::FILE *file = [] {
		const char *p = std::getenv("M2_GEO_SRC_TRACE");
		return p && *p ? std::fopen(p, "w") : nullptr;
	}();
	static const uint32_t want_oba = [] {
		const char *v = std::getenv("M2_GEO_SRC_OBA");
		return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0u;
	}();
	static const uint64_t want_at = [] {
		const char *v = std::getenv("M2_GEO_SRC_AT");
		return v ? std::strtoull(v, nullptr, 10) : ~0ull;
	}();
	return file && tick == want_at && oba == want_oba ? file : nullptr;
}

static int append_source_poly(std::vector<GeoPoly> &out, const float (*turned)[5], int count, const float *matrix,
                              float focus_x, float focus_y, const Geo::plane *planes, bool quantize);

// Local play build: fir trees in volume (M2_TREE_VOLUME=1), after the way
// classic console trees were built: a few simple shells around the trunk,
// clad in the cut-out foliage picture, with the light baked per tier (dark
// low down, bright at the top). Each tree is cut into horizontal bands of its
// own half-card picture; every band is repeated around the trunk and its
// outer edge drops like a branch. Exactly one of the tree's four half-cards
// (the one whose outward direction falls in a fixed 90-degree sector of the
// object frame) builds it, in the emitting path and the pose replay alike.
// h is the texture coordinate along the height (low = top), a across
// (512 = trunk). Returns the polygons appended.
static int append_tree_volume(std::vector<GeoPoly> &out, const float (*src)[5], int count, const float *matrix,
                              float focus_x, float focus_y, const Geo::plane *planes, bool quantize,
                              const float *Du, const float *Dv, bool v_up,
                              float umin, float umax, float vmin, float vmax, uint64_t frame_key,
                              float droop_ratio)
{
	const float hmin = v_up ? vmin : umin, hmax = v_up ? vmax : umax;
	const float amin = v_up ? umin : vmin, amax = v_up ? umax : vmax;
	const float a_out = std::fabs(amin - 512) < 1 ? amax : amin;
	const float u0 = src[0][3], v0 = src[0][4];
	const auto at = [&](float h, float a, float *p) {
		const float tu = (v_up ? a : h) - u0, tv = (v_up ? h : a) - v0;
		for (int c = 0; c < 3; c++) p[c] = src[0][c] + tu * Du[c] + tv * Dv[c];
	};
	float top[3], bottom[3], outer[3];
	at(hmin, 512, top);
	at(hmax, 512, bottom);
	at(hmax, a_out, outer);
	float ax[3], o[3];
	for (int c = 0; c < 3; c++) ax[c] = bottom[c] - top[c], o[c] = outer[c] - bottom[c];
	const float tall = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
	if (tall <= 0) return 0;
	for (float &c : ax) c /= tall; // downwards
	// Fixed frame across the trunk: object X (or Z) without its axial part.
	// One tree per trunk and frame, however many half-cards it has (four for
	// a crossed pair, two for the trees set in an L against a wall): the
	// trunk top, seen from the camera, is remembered for this frame.
	{
		struct Seen { uint64_t frame = ~0ull; std::vector<std::array<int32_t, 3>> trunks; };
		thread_local Seen seen;
		if (seen.frame != frame_key) seen.frame = frame_key, seen.trunks.clear();
		GeoVertex t{};
		t.x = top[0]; t.y = top[1]; t.pz = top[2];
		transform_point(&t, matrix);
		const std::array<int32_t, 3> key{int32_t(std::lround(t.x * 8)), int32_t(std::lround(t.y * 8)),
		                                 int32_t(std::lround(t.pz * 8))};
		if (std::find(seen.trunks.begin(), seen.trunks.end(), key) != seen.trunks.end()) return 0;
		seen.trunks.push_back(key);
	}
	(void)o;
	constexpr float kPi = 3.14159265f;

	const uint8_t luma = out.back().luma;
	constexpr int kBands = 6, kAround = 6;
	int added = 0;
	for (int b = 0; b < kBands; b++) {
		// Bands from the top down to just above the foot of the trunk,
		// each reaching a little into the one above.
		const float t0 = std::max(0.0f, (b - 0.15f) / kBands) * 0.97f, t1 = float(b + 1) / kBands * 0.97f;
		const float h0 = hmin + (hmax - hmin) * t0, h1 = hmin + (hmax - hmin) * t1;
		float quad[4][5]; // top outer, bottom outer, bottom inner, top inner (the game's order)
		const float hs[4] = {h0, h1, h1, h0}, as[4] = {a_out, a_out, 512, 512};
		for (int k = 0; k < 4; k++) {
			at(hs[k], as[k], quad[k]);
			quad[k][3] = v_up ? as[k] : hs[k];
			quad[k][4] = v_up ? hs[k] : as[k];
		}
		// Baked light: the lowest tier at 72 %, the top at 100 %.
		const float shade = 1.0f - 0.28f * (t0 + t1) * 0.5f;
		for (int j = 0; j < kAround; j++) {
			const float ang = 2 * kPi * j / kAround + b * (kPi / kAround) + 0.35f;
			const float ca = std::cos(ang), sa = std::sin(ang);
			float turned[4][5];
			for (int k = 0; k < 4; k++) {
				float r[3], axial[3];
				for (int c = 0; c < 3; c++) r[c] = quad[k][c] - top[c];
				const float d = ax[0] * r[0] + ax[1] * r[1] + ax[2] * r[2];
				for (int c = 0; c < 3; c++) axial[c] = top[c] + d * ax[c], r[c] -= d * ax[c];
				const float radius = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
				const float cr[3] = {ax[1] * r[2] - ax[2] * r[1], ax[2] * r[0] - ax[0] * r[2], ax[0] * r[1] - ax[1] * r[0]};
				const float droop = droop_ratio * radius; // the outer edge hangs like a branch
				for (int c = 0; c < 3; c++) turned[k][c] = axial[c] + r[c] * ca + cr[c] * sa + droop * ax[c];
				turned[k][3] = quad[k][3];
				turned[k][4] = quad[k][4];
			}
			if (append_source_poly(out, turned, 4, matrix, focus_x, focus_y, planes, quantize)) {
				out.back().luma = uint8_t(std::min(255.0f, luma * shade));
				added++;
			}
		}
	}
	return added;
}

// Local play build: volumetric trees. The game's trees are two crossed
// cut-out cards, each cut at the trunk into two half-cards carrying half of
// the picture across (texture 0..512 and 512..1024). For a half-card using
// one of the listed textures (M2_TREE_TEX="w2:w3,...", texture header words
// 2 and 3; default the Beginner fir trees), one copy turned 45 degrees about
// the trunk is appended after the polygon just emitted (out.back()): the
// crossed pair becomes four planes, a tree from every side. The axis comes
// from the texture (the object-space steps per texture unit), so both
// triangles of a card agree. Opt-in: M2_TREE_STAR=1. Returns the polygons
// appended. Used by the emitting path and by the pose replay alike.
static int append_tree_star(std::vector<GeoPoly> &out, const float (*src)[5], int count, const float *matrix,
                            float focus_x, float focus_y, const Geo::plane *planes, bool quantize,
                            uint32_t tex_key, bool mobile, uint64_t frame_key)
{
	static const bool tree_star = [] {
		const char *v = std::getenv("M2_TREE_STAR");
		return v && v[0] == '1';
	}();
	static const std::vector<uint32_t> tree_tex = [] {
		std::vector<uint32_t> keys;
		const char *v = std::getenv("M2_TREE_TEX");
		if (!v) v = "0e10:a180,0e10:a1c0,0a28:9340,0e10:a100,0a28:de40";
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
	static const bool tree_volume = [] {
		const char *v = std::getenv("M2_TREE_VOLUME");
		return v && v[0] == '1';
	}();
	if ((!tree_star && !tree_volume) || mobile || count < 3 || out.empty() ||
		std::find(tree_tex.begin(), tree_tex.end(), tex_key) == tree_tex.end())
		return 0;
	// Object-space steps per texture unit, from the first three vertices.
	const float u0 = src[0][3], v0 = src[0][4];
	const float du1 = src[1][3] - u0, dv1 = src[1][4] - v0, du2 = src[2][3] - u0, dv2 = src[2][4] - v0;
	const float det = du1 * dv2 - du2 * dv1;
	if (std::fabs(det) < 1e-6f) return 0;
	float Du[3], Dv[3];
	for (int c = 0; c < 3; c++) {
		const float e1 = src[1][c] - src[0][c], e2 = src[2][c] - src[0][c];
		Du[c] = (e1 * dv2 - e2 * dv1) / det;
		Dv[c] = (e2 * du1 - e1 * du2) / det;
	}
	float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
	for (int k = 0; k < count; k++) {
		umin = std::min(umin, src[k][3]); umax = std::max(umax, src[k][3]);
		vmin = std::min(vmin, src[k][4]); vmax = std::max(vmax, src[k][4]);
	}
	const float lu = std::sqrt(Du[0] * Du[0] + Du[1] * Du[1] + Du[2] * Du[2]);
	const float lv = std::sqrt(Dv[0] * Dv[0] + Dv[1] * Dv[1] + Dv[2] * Dv[2]);
	const bool v_up = lv * (vmax - vmin) >= lu * (umax - umin);
	const float *up = v_up ? Dv : Du;
	const float up_len = v_up ? lv : lu;
	const float amin = v_up ? umin : vmin, amax = v_up ? umax : vmax;
	const bool half_card = (std::fabs(amin) < 1 && std::fabs(amax - 512) < 1) ||
	                       (std::fabs(amin - 512) < 1 && std::fabs(amax - 1024) < 1);
	if (!half_card || up_len <= 0) return 0;
	const float height = up_len * (v_up ? vmax - vmin : umax - umin);
	const float width = (v_up ? lu : lv) * (amax - amin) * 2; // the whole card
	if (height <= 0 || width > 0.9f * height) return 0; // a tree, not a painted row
	if (tree_volume)
		return append_tree_volume(out, src, count, matrix, focus_x, focus_y, planes, quantize, Du, Dv, v_up,
		                          umin, umax, vmin, vmax, frame_key,
		                          // the broad-leaved tree of Beginner keeps a rounder crown
		                          tex_key == 0x0a28de40u ? 0.12f : 0.3f);
	// The trunk: across 512, up at its low end.
	const float du = (v_up ? 512.0f : (v_up ? vmin : umin)) - u0, dv = (v_up ? vmin : 512.0f) - v0;
	float base[3], ax[3];
	for (int c = 0; c < 3; c++) {
		base[c] = src[0][c] + du * Du[c] + dv * Dv[c];
		ax[c] = up[c] / up_len;
	}
	const float a = 0.78539816f, ca = std::cos(a), sa = std::sin(a);
	float turned[8][5];
	for (int k = 0; k < count; k++) {
		const float r[3] = {src[k][0] - base[0], src[k][1] - base[1], src[k][2] - base[2]};
		const float d = ax[0] * r[0] + ax[1] * r[1] + ax[2] * r[2];
		const float cr[3] = {ax[1] * r[2] - ax[2] * r[1], ax[2] * r[0] - ax[0] * r[2], ax[0] * r[1] - ax[1] * r[0]};
		turned[k][0] = base[0] + r[0] * ca + cr[0] * sa + ax[0] * d * (1 - ca);
		turned[k][1] = base[1] + r[1] * ca + cr[1] * sa + ax[1] * d * (1 - ca);
		turned[k][2] = base[2] + r[2] * ca + cr[2] * sa + ax[2] * d * (1 - ca);
		turned[k][3] = src[k][3]; turned[k][4] = src[k][4];
	}
	return append_source_poly(out, turned, count, matrix, focus_x, focus_y, planes, quantize);
}

// Appends a copy of out.back() (same texture, light, sort and window) with
// new object-space vertices (x, y, z, u, v): transformed by the object
// matrix, projected and clipped like the game's own. Returns 0 or 1.
static int append_source_poly(std::vector<GeoPoly> &out, const float (*turned)[5], int count, const float *matrix,
                              float focus_x, float focus_y, const Geo::plane *planes, bool quantize)
{
	if (out.empty() || count < 3 || count > 8) return 0;
	GeoVertex vin[8], vout[8];
	GeoVertex *vi = vin, *vo = vout;
	for (int k = 0; k < count; k++) {
		GeoVertex p{};
		p.x = turned[k][0]; p.y = turned[k][1]; p.pz = turned[k][2];
		p.ox = p.x; p.oy = p.y; p.oz = p.pz;
		transform_point(&p, matrix);
		p.x *= focus_x;
		p.y *= focus_y;
		if (quantize) {
			p.x = u2f(f2u(p.x) & 0xffffff00u);
			p.y = u2f(f2u(p.y) & 0xffffff00u);
			p.pz = u2f(f2u(p.pz) & 0xffffff00u);
		}
		p.pu = turned[k][3];
		p.pv = turned[k][4];
		vi[k] = p;
	}
	int n = count;
	for (int i = 0; i < 4 && n; i++) {
		if (polygon_inside_plane(vi, n, planes[i])) continue;
		n = clip_polygon(vi, n, vo, planes[i]);
		std::swap(vi, vo);
	}
	if (n < 3) return 0;
	const GeoPoly base_poly = out.back();
	out.push_back(base_poly);
	GeoPoly &copy = out.back();
	copy.num_vertices = uint8_t(n);
	for (int i = 0; i < n; i++) copy.v[i] = vi[i];
	copy.source_num_vertices = uint8_t(count);
	for (int k = 0; k < count; k++)
		for (int c = 0; c < 5; c++) copy.source_v[k][c] = turned[k][c];
	return 1;
}

// Local play build: the open end of the Beginner main grandstand. Block
// 0x805ae5 closes the triangular end of the stand with a lattice of posts and
// bars (untextured, 0000:9b80) and nothing behind it: through the squares one
// sees the screen background (sky). After the tallest post is emitted, a
// solid triangle in the same flat colour is appended just inside the lattice,
// from the ground (y 2.5) up to the top of the slope, in object space.
// Opt-in: M2_STAND_FILL=1. Used by the emitting path and the pose replay.
static int append_stand_fill(std::vector<GeoPoly> &out, const float (*src)[5], int count, const float *matrix,
                             float focus_x, float focus_y, const Geo::plane *planes, bool quantize,
                             uint32_t oba, uint32_t tex_key)
{
	static const bool enabled = [] {
		const char *v = std::getenv("M2_STAND_FILL");
		return v && v[0] == '1';
	}();
	if (!enabled || oba != 0x805ae5 || tex_key != 0x00009b80u || count != 4) return 0;
	// The tallest post: (-125.1, 2.5, -125.0) .. (-125.3, 19.0, -125.5).
	const auto near = [](float a, float b) { return std::fabs(a - b) < 0.2f; };
	if (!(near(src[0][0], -125.1f) && near(src[0][1], 2.5f) && near(src[0][2], -125.0f) &&
	      near(src[1][1], 18.7f)))
		return 0;
	// End plane, in (s along the posts' line from the first post, t into the
	// stand): the crowd slope's front edge (1a58:8a00) runs from s -1.7 at
	// y 2.5 to s 28.3 at y 22.5. The triangle follows it, half a unit up so
	// it tucks under the edge, and sits 0.6 inside the lattice.
	const auto at = [](float s, float y, float *p) {
		const float t = 0.6f;
		p[0] = -113.8f - 0.501f * s + 0.865f * t;
		p[1] = y;
		p[2] = -105.5f - 0.865f * s - 0.501f * t;
		p[3] = p[4] = 0;
	};
	float tri[3][5];
	at(-1.7f, 2.5f, tri[0]);
	at(28.3f, 23.0f, tri[1]);
	at(28.3f, 2.5f, tri[2]);
	return append_source_poly(out, tri, 3, matrix, focus_x, focus_y, planes, quantize);
}

// The same fascia anchor is used in the live and interpolated display lists.
// The rock-and-concrete version is an independent local option.
static int append_canyon_support(std::vector<GeoPoly> &out, const float (*src)[5], int count,
                                 const float *matrix, float focus_x, float focus_y,
                                 const Geo::plane *planes, bool quantize,
                                 uint32_t oba, uint32_t tex_key)
{
	static const bool enabled = [] {
		const char *value = std::getenv("M2_CANYON_FILL");
		return value && value[0] == '1';
	}();
	static const bool mixed = [] {
		const char *value = std::getenv("M2_CANYON_ROCK_ABUTMENT");
		return value && value[0] == '1';
	}();
	if ((!enabled && !mixed) || oba != 0x82f2ef || tex_key != 0x0c0c9580u || count != 4) return 0;
	if (std::fabs(src[0][0] - 183.095f) > 0.02f ||
	    std::fabs(src[0][1] - 121.850f) > 0.02f ||
	    std::fabs(src[0][2] - 126.091f) > 0.02f ||
	    std::fabs(src[1][1] - 122.850f) > 0.02f) return 0;

	// Polygon ordering can put the sides of the supports in front of the road.
	// The supports sit below the deck and are seen from its underside.
	GeoVertex deck{}, up{};
	deck.y = src[0][1];
	up.y = 1.0f;
	transform_point(&deck, matrix);
	transform_vector(&up, matrix);
	if (dot_product(up, deck) <= 0.0f) return 0;

	constexpr auto palette = [](int red, int green, int blue) {
		return uint16_t(red | (green << 5) | (blue << 10));
	};
	struct Ring {
		float y, half_length, half_width;
		float shift = 0.0f, recess = 0.0f;
		uint16_t color = 0;
	};
	// The deck's driving surface is at local Y 121.85. Keep the piers below it.
	constexpr float pier_top = 121.80f;
	constexpr Ring original[] = {
		{-104.0f, 9.0f, 5.8f}, {-91.0f, 7.4f, 4.5f},
		{110.0f, 3.5f, 2.1f}, {116.0f, 5.5f, 3.1f}, {pier_top, 5.5f, 3.1f},
	};
	constexpr Ring concrete[] = {
		{-110.0f, 11.2f, 7.1f, 0, 0, palette(14, 13, 11)},
		{-100.0f, 11.2f, 7.1f, 0, 0, palette(15, 14, 12)},
		{-91.0f, 7.4f, 4.7f, 0, 0, palette(17, 16, 14)},
		{-67.0f, 7.0f, 4.4f, 0, 0, palette(17, 16, 14)},
		{96.0f, 3.9f, 2.5f, 0, 0, palette(19, 18, 16)},
		{114.0f, 5.9f, 3.4f, 0, 0, palette(18, 17, 15)},
		{pier_top, 5.9f, 3.4f, 0, 0, palette(20, 19, 17)},
	};
	// The visible end tapers into the deck. The wider lower rings extend
	// into the cliff instead of growing across the open side of the span.
	constexpr Ring rock[] = {
		{-112.0f, 24.0f, 14.0f, -2.0f, 4.0f, palette(16, 12, 8)},
		{-53.0f, 22.0f, 13.0f, 1.0f, 3.0f, palette(18, 14, 10)},
		{-14.0f, 18.0f, 14.0f, -1.0f, 2.0f, palette(21, 16, 12)},
		{22.0f, 15.0f, 11.0f, 1.0f, 2.0f, palette(18, 14, 10)},
		{52.0f, 14.0f, 10.0f, 0.0f, 1.0f, palette(21, 17, 12)},
		{87.0f, 12.0f, 8.0f, 2.0f, 1.0f, palette(20, 16, 11)},
		{120.9f, 10.0f, 6.0f, 4.0f, 0.0f, palette(22, 17, 12)},
	};
	constexpr float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	const uint8_t stone_light = out.back().luma;
	int added = 0;
	auto append_shape = [&](const auto &rings, float distance, float side_x, float side_z) {
		// The fascia runs (0.5, -0.866) in the object frame.
		const float cx = src[0][0] + 0.5f * distance + side_x;
		const float cz = src[0][2] - 0.8660254f * distance + side_z;
		for (size_t level = 1; level < std::size(rings); ++level) {
			for (size_t side = 0; side < std::size(corners); ++side) {
				auto vertex = [&](float (&v)[5], const Ring &ring, size_t corner, float u) {
					const float along = corners[corner][0] * ring.half_length + ring.shift;
					const float across = corners[corner][1] * ring.half_width - ring.recess;
					v[0] = cx + 0.5f * along + 0.8660254f * across;
					v[1] = ring.y;
					v[2] = cz - 0.8660254f * along + 0.5f * across;
					v[3] = u;
					v[4] = (122.1f - ring.y) * 16.0f;
				};
				const size_t next = (side + 1) % std::size(corners);
				float face[4][5];
				vertex(face[0], rings[level - 1], next, 256.0f);
				vertex(face[1], rings[level - 1], side, 0.0f);
				vertex(face[2], rings[level], side, 0.0f);
				vertex(face[3], rings[level], next, 256.0f);
				GeoVertex camera[3];
				for (int i = 0; i < 3; ++i) {
					camera[i].x = face[i][0];
					camera[i].y = face[i][1];
					camera[i].pz = face[i][2];
					transform_point(&camera[i], matrix);
				}
				GeoVertex normal;
				vector_cross3(&normal, &camera[0], &camera[1], &camera[2]);
				if (dot_product(normal, camera[0]) >= 0.0f) continue;
				if (!append_source_poly(out, face, 4, matrix, focus_x, focus_y, planes, quantize)) continue;
				auto &poly = out.back();
				const uint16_t color = rings[level - 1].color;
				if (color) {
					poly.texheader[0] = 0;
					poly.local_solid_palette = color;
					poly.luma = uint8_t(side % 2 ? 151 : 116);
				} else {
					poly.luma = uint8_t(side % 2 ? stone_light : stone_light * 7 / 8);
				}
				++added;
			}
		}
	};
	for (const float distance : {15.0f, 50.0f}) {
		if (mixed) append_shape(concrete, distance, -2.6f, -1.5f);
		else append_shape(original, distance, -2.6f, -1.5f);
	}
	if (mixed) append_shape(rock, 95.0f, -8.660254f, -5.0f);
	return added;
}

template <unsigned NumVerts>
void Geo::model2_3d_process_polygon(raster_state *raster, uint32_t attr)
{
#ifdef M2_FAST_GEO
	// Not zero-filled first (GeoVertex's initialisers, for every polygon):
	// every field used below is written before it is read.
	union ObjectStorage { ObjectStorage() {} quad_m2 q; } object_storage;
	quad_m2 &object = object_storage.q;
#else
	quad_m2 object;
#endif
	GeoPtr16 th, tp;
	int32_t tho;
	uint32_t i;
	bool cull;
	float zvalue;
	float min_z, max_z;
	float source_v[4][5] = {};

	static_assert(NumVerts == 3 || NumVerts == 4, "Polygon must have 3 or 4 vertices");
	m2trace::count_vertices(NumVerts);

	/* extract P0(n-1) */
	object.v[1].x = u2f(raster->command_buffer[2] << 8);
	object.v[1].y = u2f(raster->command_buffer[3] << 8);
	object.v[1].pz = u2f(raster->command_buffer[4] << 8);

	/* extract P1(n-1) */
	object.v[0].x = u2f(raster->command_buffer[5] << 8);
	object.v[0].y = u2f(raster->command_buffer[6] << 8);
	object.v[0].pz = u2f(raster->command_buffer[7] << 8);

	/* extract P0(n) */
	object.v[2].x = u2f(raster->command_buffer[11] << 8);
	object.v[2].y = u2f(raster->command_buffer[12] << 8);
	object.v[2].pz = u2f(raster->command_buffer[13] << 8);

	if (NumVerts == 4)
	{
		/* extract P1(n) */
		object.v[3].x = u2f(raster->command_buffer[14] << 8);
		object.v[3].y = u2f(raster->command_buffer[15] << 8);
		object.v[3].pz = u2f(raster->command_buffer[16] << 8);
	}
	else
	{
		/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
		raster->command_buffer[14] = raster->command_buffer[11];
		raster->command_buffer[15] = raster->command_buffer[12];
		raster->command_buffer[16] = raster->command_buffer[13];
		for (int k = 0; k < 3; ++k)
			raster->object_pos[14][k] = raster->object_pos[11][k];
	}

	/* keep the pre-matrix positions alongside the transformed ones */
	object.v[0].ox = raster->object_pos[5][0]; object.v[0].oy = raster->object_pos[5][1]; object.v[0].oz = raster->object_pos[5][2];
	object.v[1].ox = raster->object_pos[2][0]; object.v[1].oy = raster->object_pos[2][1]; object.v[1].oz = raster->object_pos[2][2];
	object.v[2].ox = raster->object_pos[11][0]; object.v[2].oy = raster->object_pos[11][1]; object.v[2].oz = raster->object_pos[11][2];
	if (NumVerts == 4) { object.v[3].ox = raster->object_pos[14][0]; object.v[3].oy = raster->object_pos[14][1]; object.v[3].oz = raster->object_pos[14][2]; }

	/* always calculate the min z and max z value */
	min_z = object.v[0].pz;
	if (object.v[1].pz < min_z) min_z = object.v[1].pz;
	if (object.v[2].pz < min_z) min_z = object.v[2].pz;
	if (NumVerts == 4 && object.v[3].pz < min_z) min_z = object.v[3].pz;

	max_z = object.v[0].pz;
	if (object.v[1].pz > max_z) max_z = object.v[1].pz;
	if (object.v[2].pz > max_z) max_z = object.v[2].pz;
	if (NumVerts == 4 && object.v[3].pz > max_z) max_z = object.v[3].pz;

	/* read in the texture information */

	/* texture point data */
	if (raster->command_buffer[0] & 0x800000)
		tp = GeoPtr16{raster->texture_ram, 0x10000, raster->command_buffer[0] & 0xffff};
	else
#ifdef M2_DC_MEMORY
		tp = GeoPtr16{raster->texture_rom, raster->texture_rom_mask + 1, raster->command_buffer[0] & raster->texture_rom_mask, raster->rom};
#else
		tp = GeoPtr16{raster->texture_rom, raster->texture_rom_mask + 1, raster->command_buffer[0] & raster->texture_rom_mask};
#endif

	/* texture header data */
	if (raster->command_buffer[1] & 0x800000)
		th = GeoPtr16{raster->texture_ram, 0x10000, raster->command_buffer[1] & 0xffff};
	else
#ifdef M2_DC_MEMORY
		th = GeoPtr16{raster->texture_rom, raster->texture_rom_mask + 1, raster->command_buffer[1] & raster->texture_rom_mask, raster->rom};
#else
		th = GeoPtr16{raster->texture_rom, raster->texture_rom_mask + 1, raster->command_buffer[1] & raster->texture_rom_mask};
#endif

	if (capturing_rom_object_)
		capture_rom_primitive<NumVerts>(*raster, tp, th);
	if (instance_reference_ || pose_presentation_) {
		auto &entry = reference_primitives_.emplace_back();
		entry.rom = capturing_rom_object_;
		if (entry.rom) {
			entry.geometry = rom_object_.primitives.back();
			entry.matrix = rom_object_.matrix;
		} else {
			entry.geometry.vertex_count = NumVerts;
			std::copy_n(cur_matrix, 12, entry.matrix.begin());
			auto uv_cursor = tp;
			auto header_cursor = th;
			for (unsigned k = 0; k < NumVerts; ++k) {
				entry.fallback_vertices[k] = object.v[k];
				entry.geometry.vertices[k].v = *uv_cursor++;
				entry.geometry.vertices[k].u = *uv_cursor++;
			}
			for (auto &word : entry.geometry.texture_header) word = *header_cursor++;
		}
		entry.attributes = attr;
		entry.focus_x = cur_focus_x;
		entry.focus_y = cur_focus_y;
		std::copy_n(raster->clip_plane[raster->center_sel], 4, entry.planes.begin());
		std::copy_n(raster->viewport, 4, entry.viewport);
		std::copy_n(raster->center[raster->center_sel], 2, entry.center);
		entry.window = raster->cur_window;
		entry.master_z_clip = raster->master_z_clip;
		entry.reverse = raster->reverse;
		entry.face_word = raster->command_buffer[9];
		entry.luma = (entry.face_word >> 15) & 0xff;
		entry.texlod = ((raster->command_buffer[10] >> 8) & 0x7f80) - 0x3f80;
		entry.texlod += raster->log_ram[raster->command_buffer[10] & 0x7fff];
		entry.z_adjust = raster->z_adjust;
		entry.oba = cur_oba;
		entry.obc = cur_obc;
		entry.command = cur_object_command;
		entry.mode = cur_mode;
		entry.mobile_key = current_mobile_key_;
		entry.direct = cur_direct;
	}

	/* update the address */
	raster->command_buffer[0] += NumVerts * 2;

#ifdef M2_FAST_GEO
	// Read texture data below, for a polygon not culled.
#else
	for (auto i = 0u; i < NumVerts; ++i) {
		object.v[i].pv = *tp++;
		object.v[i].pu = *tp++;
	}
	object.texheader[0] = *th++;
	object.texheader[1] = *th++;
	object.texheader[2] = *th++;
	object.texheader[3] = *th++;
#endif

	/* extract the texture header offset */
	tho = (attr >> 12) & 0x1f;

	/* adjust for sign */
	if (tho & 0x10)
		tho |= -16;

	/* update the address */
	raster->command_buffer[1] += tho * 4;

	/* set the luma value of this polygon */
	object.luma = (raster->command_buffer[9] >> 15) & 0xff;

#ifndef M2_FAST_GEO
	/* set the texture LOD of this polygon */
	object.texlod = ((raster->command_buffer[10] >> 8) & 0x7f80) - 0x3f80;
	object.texlod += raster->log_ram[raster->command_buffer[10] & 0x7fff];
#endif

	/* determine whether we can cull this polygon */
	cull = check_culling(raster,attr,min_z,max_z);

	// Ray-traced shadows: wholly behind the camera, it is still kept apart
	// as a shadow caster (Geo::shadow_casters), whichever way it faces.
	if (cull && top_margin_ && max_z < 0 && min_z > -kCasterReach && ((attr >> 8) & 3) != 0 && behind_casters_on())
	{
		GeoPoly &caster = shadow_casters_.emplace_back();
#ifdef M2_FAST_GEO
		{
			GeoPtr16 tc = tp, hc = th;
			uint16_t uv[NumVerts * 2];
			tc.read(uv, NumVerts * 2);
			for (uint32_t k = 0; k < NumVerts; k++)
			{
				object.v[k].pv = uv[2 * k];
				object.v[k].pu = uv[2 * k + 1];
			}
			hc.read(object.texheader, 4);
		}
#endif
		for (int k = 0; k < 4; k++) caster.texheader[k] = object.texheader[k];
		caster.luma = object.luma;
		for (int k = 0; k < 4; k++) caster.viewport[k] = raster->viewport[k];
		caster.center[0] = raster->center[raster->center_sel][0];
		caster.center[1] = raster->center[raster->center_sel][1];
		caster.window = raster->cur_window;
		caster.object_oba = cur_oba;
		caster.mobile_key = current_mobile_key_;
		caster.object_focus_x = cur_focus_x;
		caster.object_focus_y = cur_focus_y;
		caster.num_vertices = NumVerts;
		for (unsigned k = 0; k < NumVerts; k++) caster.v[k] = object.v[k];
	}

	/* set the object's z value */
	switch ((attr >> 10) & 3)
	{
		case 0: // old value
			zvalue = raster->polygon_z;
			break;
		case 1: // min z
			zvalue = min_z;
			break;
		case 2: // max z
			zvalue = max_z;
			break;
		case 3: // error
		default:
			zvalue = 1e10;
			break;
	}

	raster->polygon_z = zvalue;

	if (cull == false)
	{
		int32_t clipped_verts;
#ifdef M2_FAST_GEO
		// (As object: the clipper writes each vertex before reading it.)
		union VertexStorage { VertexStorage() {} GeoVertex v[2][8]; } vertex_storage;
		GeoVertex (&vertices)[2][8] = vertex_storage.v;
#else
		GeoVertex vertices[2][8];
#endif
		GeoVertex *verts_in = vertices[0], *verts_out = vertices[1];

#ifdef M2_FAST_GEO
		// Only a polygon not culled reads its texture coordinates, texture
		// header and LOD (nothing above uses them; tp and th are where they
		// were set, before the address updates): the same values.
		{
			uint16_t uv[NumVerts * 2]; // (as a block)
			tp.read(uv, NumVerts * 2);
			for (uint32_t k = 0; k < NumVerts; k++)
			{
				object.v[k].pv = uv[2 * k];
				object.v[k].pu = uv[2 * k + 1];
			}
		}
		th.read(object.texheader, 4);
		object.texlod = ((raster->command_buffer[10] >> 8) & 0x7f80) - 0x3f80;
		object.texlod += raster->log_ram[raster->command_buffer[10] & 0x7fff];
#endif

		for (int i = 0; i < NumVerts; i++) {
			verts_in[i] = object.v[i];
			source_v[i][0] = object.v[i].ox;
			source_v[i][1] = object.v[i].oy;
			source_v[i][2] = object.v[i].oz;
			source_v[i][3] = object.v[i].p[1];
			source_v[i][4] = object.v[i].p[2];
		}

		clipped_verts = NumVerts;
		bool cut = false;

		/* do clipping */
		for (int i = 0; i < 4 && clipped_verts; i++)
		{
			const auto &clip_plane = raster->clip_plane[raster->center_sel][i];
			// Most polygons are wholly inside a plane. Preserve the same dot
			// comparisons while avoiding the edge walk and vertex copies.
			if (polygon_inside_plane(verts_in, clipped_verts, clip_plane))
				continue;
			cut = true;
			clipped_verts = clip_polygon(verts_in, clipped_verts, verts_out, clip_plane);
			std::swap(verts_in, verts_out);
		}

		// Ray-traced shadows: a polygon near the camera that the view's edges
		// cut (a canyon wall or a row of trees running past the car): only its
		// part inside the view was in the frame, so the shadow it casts beside
		// the car was lost and the shadow's edge walked ahead of the car, a
		// "wall of light" (23/11). Its parts outside the view become casters.
		if (cut && top_margin_ && min_z > -cut_reach() && min_z < cut_reach() && behind_casters_on())
			outside_parts(object.v, NumVerts, raster->clip_plane[raster->center_sel], 4,
				[&](const GeoVertex *part, int n) {
					GeoPoly &caster = shadow_casters_.emplace_back();
					for (int k = 0; k < 4; k++) caster.texheader[k] = object.texheader[k];
					caster.luma = object.luma;
					for (int k = 0; k < 4; k++) caster.viewport[k] = raster->viewport[k];
					caster.center[0] = raster->center[raster->center_sel][0];
					caster.center[1] = raster->center[raster->center_sel][1];
					caster.window = raster->cur_window;
					caster.object_oba = cur_oba;
					caster.mobile_key = current_mobile_key_;
					caster.object_focus_x = cur_focus_x;
					caster.object_focus_y = cur_focus_y;
					caster.num_vertices = uint8_t(n);
					for (int k = 0; k < n; k++) caster.v[k] = part[k];
				});

		if (clipped_verts > 2)
		{
			/* adjust and set the object z-sort value */
			object.z = z_sort_value(zvalue, raster->z_adjust);

			/* go through the clipped vertex list, adding polygons */
			raster->poly_list_index++;
			if (raster->poly_list_index >= 32768) // MAME MAX_POLYGONS
				throw GeoFatal("SEGA 3D: Max polygon limit exceeded");
			polys.emplace_back();
			GeoPoly *poly = &polys.back();
			poly->reverse = raster->reverse;

			/* copy the object information */
			poly->z = object.z;
			poly->object_z_adjust = raster->z_adjust;
			poly->texheader[0] = object.texheader[0];
			poly->texheader[1] = object.texheader[1];
			poly->texheader[2] = object.texheader[2];
			poly->texheader[3] = object.texheader[3];
			poly->luma = object.luma;
			poly->texlod = object.texlod;

			/* set the viewport */
			poly->viewport[0] = raster->viewport[0];
			poly->viewport[1] = raster->viewport[1];
			poly->viewport[2] = raster->viewport[2];
			poly->viewport[3] = raster->viewport[3];

			/* set the center */
			poly->center[0] = raster->center[raster->center_sel][0];
			poly->center[1] = raster->center[raster->center_sel][1];

			/* set the window */
			poly->window = raster->cur_window;

			poly->num_vertices = clipped_verts;
			poly->source_num_vertices = NumVerts;
			std::memcpy(poly->source_v, source_v, sizeof source_v);
			poly->object_oba = cur_oba;
			poly->object_obc = cur_obc;
			poly->object_command = cur_object_command;
			poly->mobile_key = current_mobile_key_;
			poly->object_mode = cur_mode;
			poly->object_direct = cur_direct;
			std::memcpy(poly->object_matrix, cur_matrix, sizeof cur_matrix);
			poly->object_focus_x = cur_focus_x;
			poly->object_focus_y = cur_focus_y;

			for (int i = 0; i < clipped_verts; i++)
				poly->v[i] = verts_in[i];

			// (MAME links it into a per-z bucket here; the display list keeps
			// the order added, from which the bucket order follows.)

			/* keep around the min and max z values for this frame */
			if (object.z < raster->min_z) raster->min_z = object.z;
			if (object.z > raster->max_z) raster->max_z = object.z;

			// Diagnostic, shared by every renderer (m2run too): cut-out
			// textured polygons (renderer 3). M2_GEO_CUTOUT_TRACE=FILE logs
			// tick, object, texture words and the projected centre (x/z, y/z);
			// M2_GEO_CUTOUT_FROM=N starts at tick N. M2_GEO_HIDE_TEX=w2:w3,...
			// drops those textures' polygons here, before any renderer.
			{
				static std::FILE *cut_trace = [] {
					const char *p = std::getenv("M2_GEO_CUTOUT_TRACE");
					return p && *p ? std::fopen(p, "w") : nullptr;
				}();
				static const uint64_t cut_from = [] {
					const char *v = std::getenv("M2_GEO_CUTOUT_FROM");
					return v ? std::strtoull(v, nullptr, 10) : 0ull;
				}();
				static const std::vector<uint32_t> hide_tex = [] {
					std::vector<uint32_t> keys;
					if (const char *v = std::getenv("M2_GEO_HIDE_TEX"))
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
				const GeoPoly &emitted = polys.back();
				const uint32_t key = uint32_t(emitted.texheader[2]) << 16 | emitted.texheader[3];
				static const bool cut_all = [] { // M2_GEO_CUTOUT_ALL=1: every polygon, not only cut-outs
					const char *v = std::getenv("M2_GEO_CUTOUT_ALL");
					return v && v[0] == '1';
				}();
				if (cut_trace && tick_ >= cut_from && (cut_all || ((emitted.texheader[0] >> 13) & 3) == 3)) {
					float sx = 0, sy = 0, sz = 0;
					for (int i = 0; i < emitted.num_vertices; i++) sx += emitted.v[i].x, sy += emitted.v[i].y, sz += emitted.v[i].pz;
					std::fprintf(cut_trace, "%llu,%x,%04x,%04x,%04x,%04x,%.1f,%.1f,%.2f\n", (unsigned long long)tick_, cur_oba,
					             emitted.texheader[0], emitted.texheader[1], emitted.texheader[2], emitted.texheader[3],
					             sz != 0 ? sx / sz : 0.0f, sz != 0 ? sy / sz : 0.0f, sz / float(std::max<int>(emitted.num_vertices, 1)));
				}
				// M2_GEO_SRC_TRACE=FILE with M2_GEO_SRC_OBA=hex and M2_GEO_SRC_AT=N:
				// every polygon of that object at tick N, with its object-space
				// vertices (x, y, z, u, v) and the object matrix (12 floats).
				{
					std::FILE *src_trace = geo_src_trace(tick_, cur_oba);
					if (src_trace) {
						std::fprintf(src_trace, "%x,%d,%04x,%04x,%u", cur_oba, (emitted.texheader[0] >> 13) & 3,
						             emitted.texheader[2], emitted.texheader[3], NumVerts);
						for (unsigned k = 0; k < NumVerts; k++)
							std::fprintf(src_trace, ",%.3f,%.3f,%.3f,%.1f,%.1f", source_v[k][0], source_v[k][1],
							             source_v[k][2], source_v[k][3], source_v[k][4]);
						std::fprintf(src_trace, ",M");
						for (int k = 0; k < 12; k++) std::fprintf(src_trace, ",%.4f", cur_matrix[k]);
						std::fprintf(src_trace, "\n");
						std::fflush(src_trace);
					}
				}
				if (!hide_tex.empty() && std::find(hide_tex.begin(), hide_tex.end(), key) != hide_tex.end()) {
					polys.pop_back();
					raster->poly_list_index--;
					goto after_emit;
				}
			}

			// Local play build: volumetric trees (append_tree_star, above).
			{
				const uint32_t tex_key = uint32_t(object.texheader[2]) << 16 | object.texheader[3];
				int added = append_tree_star(polys, source_v, NumVerts, cur_matrix, cur_focus_x, cur_focus_y,
				                             raster->clip_plane[raster->center_sel], false, tex_key,
				                             current_mobile_key_.part != 0, tick_);
				added += append_stand_fill(polys, source_v, NumVerts, cur_matrix, cur_focus_x, cur_focus_y,
				                           raster->clip_plane[raster->center_sel], false, cur_oba, tex_key);
				added += append_canyon_support(polys, source_v, NumVerts, cur_matrix, cur_focus_x, cur_focus_y,
				                               raster->clip_plane[raster->center_sel], false, cur_oba, tex_key);
				for (int i = 0; i < added; i++)
					if (++raster->poly_list_index >= 32768) throw GeoFatal("SEGA 3D: Max polygon limit exceeded");
			}
			after_emit:;
		}
	}

	/* update linking */
	switch (((attr >> 8) & 3))
	{
		case 0:
		case 2:
		{
			/* reuse P0(n) and P1(n) */
			for (i = 0; i < 6; i++) {                                      /* P0(n) -> P0(n-1) */
				raster->command_buffer[2+i] = raster->command_buffer[11+i]; /* P1(n) -> P1(n-1) */
				for (int k = 0; k < 3; k++) raster->object_pos[2+i][k] = raster->object_pos[11+i][k];
			}
		}
		break;

		case 1:
		{
			/* reuse P0(n-1) and P0(n) */
			for (i = 0; i < 3; i++) {
				raster->command_buffer[5+i] = raster->command_buffer[11+i]; /* P0(n) -> P1(n-1) */
				for (int k = 0; k < 3; k++) raster->object_pos[5+i][k] = raster->object_pos[11+i][k];
			}
		}
		break;

		case 3:
		{
			/* reuse P1(n-1) and P1(n) */
			for (i = 0; i < 3; i++) {
				raster->command_buffer[2+i] = raster->command_buffer[14+i]; /* P1(n) -> P1(n-1) */
				for (int k = 0; k < 3; k++) raster->object_pos[2+i][k] = raster->object_pos[14+i][k];
			}
		}
		break;
	}
}

/***********************************************************************************************/


// Polygons built in polygon RAM (no ROM model) were drawn at the latest
// tick in every interpolated frame while the ROM parts around them moved:
// the player's windshield reflection flickered until it was bound to its
// glass (above), and the opponents showed the same symptom as a shaking,
// doubled car (seen in play, 22/11). Bind every RAM emission drawn with exactly the
// matrix and projection of one ROM command to that command's sampled pose.
void Geo::bind_ram_emissions() {
	// The opponents' windshield reflections (oba 0x91 on the near body 0x8808,
	// 0x10a on the mid body 0x8c00, texture 0x4409 as the player's) share the
	// body's matrix to float rounding, not bit for bit: compare within 1e-4.
	const auto same_matrix = [](const std::array<float, 12> &a, const std::array<float, 12> &b) {
		for (size_t j = 0; j < a.size(); ++j)
			if (std::abs(a[j] - b[j]) > 1e-4f) return false;
		return true;
	};
	// The ROM primitives of one emission follow each other with the same
	// command, part, window and matrix: compare against one of each run. With
	// every opponent at the near model (40-car start) the full list made this
	// pass quadratic: frames of 30 to 90 ms (22/11).
	std::vector<const ReferencePrimitive *> roms;
	for (const auto &rom : reference_primitives_) {
		if (!rom.rom || !rom.command) continue;
		if (!roms.empty()) {
			const auto &last = *roms.back();
			if (last.command == rom.command && last.mobile_key.part == rom.mobile_key.part &&
			    last.window == rom.window && last.matrix == rom.matrix) continue;
		}
		roms.push_back(&rom);
	}
	unsigned ram = 0, bound = 0;
	for (auto &p : reference_primitives_) {
		if (p.rom || !p.command) continue;
		++ram;
		if (p.reflection_pose_command) { ++bound; continue; }
		// A car's body and its other parts are emitted with one matrix: the
		// body's command wins; otherwise the match must be unique.
		uint32_t command = 0, body = 0, first = 0;
		bool unique = true, all_unkeyed = true;
		for (const auto *rom_entry : roms) {
			const auto &rom = *rom_entry;
			if (rom.window != p.window || !same_matrix(rom.matrix, p.matrix)) continue;
			if (is_opponent_body_part(rom.mobile_key.part)) body = body && body != rom.command ? ~0u : rom.command;
			if (rom.mobile_key.part) all_unkeyed = false;
			if (command && command != rom.command) unique = false;
			if (!first) first = rom.command;
			command = rom.command;
		}
		if (body && body != ~0u) command = body, unique = true;
		// A far car is not keyed: its body is several unkeyed ROM commands with
		// one matrix, all drawn with the same scenery pose. Its reflection (oba
		// 0x91) follows the first; left on its own it was drawn at the latest
		// tick, 28 per tick over a 5-lap run: a windshield coming off (22/11).
		else if (!unique && all_unkeyed) command = first, unique = true;
		if (command && unique) {
			p.reflection_pose_command = command;
			++bound;
		}
	}
	static std::FILE *trace = [] {
		const char *path = std::getenv("M2_RAM_BIND_TRACE");
		return path && *path ? std::fopen(path, "w") : nullptr;
	}();
	if (trace) std::fprintf(trace, "%llu,%u,%u\n", (unsigned long long)tick_, ram, bound);
	if (trace && ram > bound)
		for (const auto &p : reference_primitives_) {
			if (p.rom || !p.command || p.reflection_pose_command) continue;
			// Nearest ROM command origin, and whether its rotation matches.
			float best = 1e9f; uint32_t best_part = 0; float rot = 1e9f;
			for (const auto &r : reference_primitives_) {
				if (!r.rom || !r.command) continue;
				const float d = std::hypot(r.matrix[9] - p.matrix[9], r.matrix[10] - p.matrix[10], r.matrix[11] - p.matrix[11]);
				if (d < best) {
					best = d; best_part = r.mobile_key.part; rot = 0;
					for (int j = 0; j < 9; ++j) rot = std::max(rot, std::abs(r.matrix[j] - p.matrix[j]));
				}
			}
			std::fprintf(trace, "  ram oba=%x obc=%x mode=%u direct=%u tex=%x z=%.2f part=%x key=%x near=%.3f nearpart=%x rot=%.4f\n",
			             p.oba, p.obc, p.mode, p.direct, p.geometry.texture_header[0], p.matrix[11], p.mobile_key.part,
			             p.mobile_key.slot, best, best_part, rot);
		}
}

void Geo::bind_windshield_reflection() {
    // This polygon RAM emission is the player windshield reflection on Revision A.
    const auto is_reflection = [](const ReferencePrimitive &p) {
        return !p.rom && p.oba == 4 && p.mode == 1 && p.direct == 0 &&
               p.geometry.texture_header[0] == 0x4409 && p.geometry.texture_header[2] == 7064;
    };
    const auto same_projection = [](const ReferencePrimitive &a, const ReferencePrimitive &b) {
        return a.matrix == b.matrix && a.window == b.window && a.focus_x == b.focus_x &&
               a.focus_y == b.focus_y &&
               std::equal(std::begin(a.viewport), std::end(a.viewport), std::begin(b.viewport)) &&
               std::equal(std::begin(a.center), std::end(a.center), std::begin(b.center));
    };

    const ReferencePrimitive *reflection = nullptr;
    for (const auto &p : reference_primitives_) {
        if (!is_reflection(p)) continue;
        if (!p.command || (reflection &&
            (p.command != reflection->command || !same_projection(p, *reflection)))) return;
        reflection = &p;
    }
    if (!reflection) return;

    uint32_t glass_command = 0;
    for (const auto &p : reference_primitives_) {
        if (!p.rom || p.mobile_key.slot != 0 || p.mobile_key.part != 0x7d94 ||
            !p.command || p.mode != reflection->mode || p.direct != reflection->direct ||
            !same_projection(p, *reflection)) continue;
        if (glass_command && glass_command != p.command) return;
        glass_command = p.command;
    }
    if (!glass_command) return;

    for (auto &p : reference_primitives_) {
        if (p.command == reflection->command && is_reflection(p))
            p.reflection_pose_command = glass_command;
    }
}

void Geo::replay_instance_reference(std::vector<GeoPoly> &output, bool recalculate_facing,
                                    std::span<const std::array<float, 12>> poses)
{
	output.clear();
	static std::atomic<uint64_t> replay_count{0}; // a frame key for the trees, apart from the ticks
	const uint64_t replay_key = (1ull << 63) | replay_count.fetch_add(1, std::memory_order_relaxed);
	float polygon_z = 1e10f;
	for (auto &entry : reference_primitives_) {
		const uint32_t pose_command = entry.rom ? entry.command : entry.reflection_pose_command;
		const bool has_pose = pose_command && pose_command <= poses.size();
		const auto &matrix = has_pose ? poses[pose_command - 1] : entry.matrix;
		GeoVertex banks[2][8];
		GeoVertex camera_space[3];
		GeoVertex *verts = banks[0], *out = banks[1];
		const int count = entry.geometry.vertex_count;
		for (int n = 0; n < count; ++n) {
			const auto &source = entry.geometry.vertices[n];
			auto &vertex = verts[n];
			if (entry.rom || has_pose) {
				vertex.x = entry.rom ? source.position[0] : entry.fallback_vertices[n].ox;
				vertex.y = entry.rom ? source.position[1] : entry.fallback_vertices[n].oy;
				vertex.pz = entry.rom ? source.position[2] : entry.fallback_vertices[n].oz;
				keep_object_position(&vertex);
				transform_point(&vertex, matrix.data());
				if (recalculate_facing && n < 3) camera_space[n] = vertex;
				vertex.x *= entry.focus_x;
				vertex.y *= entry.focus_y;
				vertex.x = u2f(f2u(vertex.x) & 0xffffff00u);
				vertex.y = u2f(f2u(vertex.y) & 0xffffff00u);
				vertex.pz = u2f(f2u(vertex.pz) & 0xffffff00u);
			} else {
				vertex = entry.fallback_vertices[n];
			}
			vertex.pu = source.u;
			vertex.pv = source.v;
		}
		float min_z = verts[0].pz, max_z = min_z;
		for (int n = 1; n < count; ++n) {
			if (verts[n].pz < min_z) min_z = verts[n].pz;
			if (verts[n].pz > max_z) max_z = verts[n].pz;
		}

		switch ((entry.attributes >> 10) & 3) {
		case 1: polygon_z = min_z; break;
		case 2: polygon_z = max_z; break;
		case 3: polygon_z = 1e10f; break;
		default: break;
		}
		bool back_facing = (entry.face_word & 0x00800000u) != 0;
		if (entry.rom && recalculate_facing && (entry.attributes & (1u << 17)) == 0) {
			GeoVertex normal;
			if (entry.geometry.has_rom_normal) {
				const auto &source = entry.geometry.normal;
				normal.x = source[0];
				normal.y = source[1];
				normal.pz = source[2];
				transform_vector(&normal, matrix.data());
			} else {
				// The original stream orders its first two points as P0, P1.
				vector_cross3(&normal, &camera_space[1], &camera_space[0], &camera_space[2]);
				normalize_vector(&normal);
			}
			back_facing = !(dot_product(normal, camera_space[2]) >= 0);
		}
		if (replay_casters_ && max_z < 0 && min_z > -kCasterReach && ((entry.attributes >> 8) & 3) != 0) {
			GeoPoly &caster = replay_casters_->emplace_back(); // Geo::shadow_casters, as in the game's frame
			std::copy(entry.geometry.texture_header.begin(), entry.geometry.texture_header.end(), caster.texheader);
			caster.luma = entry.luma;
			std::copy_n(entry.viewport, 4, caster.viewport);
			std::copy_n(entry.center, 2, caster.center);
			caster.window = entry.window;
			caster.object_oba = entry.oba;
			caster.mobile_key = entry.mobile_key;
			caster.object_focus_x = entry.focus_x;
			caster.object_focus_y = entry.focus_y;
			caster.num_vertices = uint8_t(count);
			std::copy_n(verts, count, caster.v);
			continue;
		}
		if (((entry.attributes & (1u << 17)) == 0 && back_facing) ||
			((entry.attributes >> 8) & 3) == 0 ||
			(entry.master_z_clip != 0xff && !far_clip_off() && int32_t(1.0 / min_z) > entry.master_z_clip) || max_z < 0)
			continue;

		int clipped = count;
		GeoVertex whole[8];
		bool cut = false, copied = false;
		for (size_t p = 0; p < entry.planes.size(); ++p) {
			const auto &plane = entry.planes[p];
			if (!clipped) break;
			if (polygon_inside_plane(verts, clipped, plane)) continue;
			if (!copied) std::copy_n(verts, count, whole), copied = true;
			cut = true;
			clipped = clip_polygon(verts, clipped, out, plane);
			std::swap(verts, out);
		}
		// As in the game's frame: cut by the view's edges near the camera, its
		// parts outside the view become shadow casters (a wall running past the car).
		if (cut && replay_casters_ && min_z > -cut_reach() && min_z < cut_reach())
			outside_parts(whole, count, entry.planes.data(), int(entry.planes.size()),
				[&](const GeoVertex *part, int n) {
					GeoPoly &caster = replay_casters_->emplace_back();
					std::copy(entry.geometry.texture_header.begin(), entry.geometry.texture_header.end(), caster.texheader);
					caster.luma = entry.luma;
					std::copy_n(entry.viewport, 4, caster.viewport);
					std::copy_n(entry.center, 2, caster.center);
					caster.window = entry.window;
					caster.object_oba = entry.oba;
					caster.mobile_key = entry.mobile_key;
					caster.object_focus_x = entry.focus_x;
					caster.object_focus_y = entry.focus_y;
					caster.num_vertices = uint8_t(n);
					std::copy_n(part, n, caster.v);
				});
		if (clipped < 3) continue;
		auto &poly = output.emplace_back();
		poly.reverse = entry.reverse;
		poly.z = z_sort_value(polygon_z, entry.z_adjust);
		poly.object_z_adjust = entry.z_adjust;
		std::copy(entry.geometry.texture_header.begin(), entry.geometry.texture_header.end(), poly.texheader);
		poly.luma = entry.luma;
		poly.texlod = entry.texlod;
		std::copy_n(entry.viewport, 4, poly.viewport);
		std::copy_n(entry.center, 2, poly.center);
		poly.window = entry.window;
		poly.num_vertices = uint8_t(clipped);
		poly.object_oba = entry.oba;
		poly.object_obc = entry.obc;
		poly.object_command = entry.command;
		poly.object_mode = entry.mode;
		poly.object_direct = entry.direct;
		std::copy(matrix.begin(), matrix.end(), poly.object_matrix);
		poly.mobile_key = entry.mobile_key;
		poly.object_focus_x = entry.focus_x;
		poly.object_focus_y = entry.focus_y;
		poly.source_num_vertices = uint8_t(count);
		for (int n = 0; n < count; ++n) {
			const auto &source = entry.geometry.vertices[n];
			for (int axis = 0; axis < 3; ++axis) poly.source_v[n][axis] = source.position[axis];
			if (!entry.rom) {
				poly.source_v[n][0] = entry.fallback_vertices[n].ox;
				poly.source_v[n][1] = entry.fallback_vertices[n].oy;
				poly.source_v[n][2] = entry.fallback_vertices[n].oz;
			}
			poly.source_v[n][3] = source.u;
			poly.source_v[n][4] = source.v;
		}
		std::copy_n(verts, clipped, poly.v);
		if (entry.rom) { // the same volumetric trees in the replayed (interpolated) scene
			float src[8][5];
			for (int n = 0; n < count; ++n)
				for (int c = 0; c < 5; ++c) src[n][c] = poly.source_v[n][c];
			const uint32_t tex_key = uint32_t(poly.texheader[2]) << 16 | poly.texheader[3];
			append_tree_star(output, src, count, matrix.data(), entry.focus_x, entry.focus_y, entry.planes.data(), true,
			                 tex_key, entry.mobile_key.part != 0, replay_key);
			append_stand_fill(output, src, count, matrix.data(), entry.focus_x, entry.focus_y, entry.planes.data(), true,
			                  entry.oba, tex_key);
			append_canyon_support(output, src, count, matrix.data(), entry.focus_x, entry.focus_y, entry.planes.data(), true,
			                      entry.oba, tex_key);
		}
	}
}

namespace {

bool rigid_background_delta(const BackgroundTransform &m) {
	if (!std::ranges::all_of(m, [](double value) { return std::isfinite(value); })) return false;
	const auto dot = [&](unsigned a, unsigned b) {
		return m[a]*m[b] + m[a+1]*m[b+1] + m[a+2]*m[b+2];
	};
	for (unsigned a = 0; a < 9; a += 3) {
		for (unsigned b = a; b < 9; b += 3) {
			if (std::abs(dot(a, b) - (a == b ? 1.0 : 0.0)) > 0.001) return false;
		}
	}
	const double determinant =
		m[0]*(m[4]*m[8] - m[5]*m[7]) - m[3]*(m[1]*m[8] - m[2]*m[7]) +
		m[6]*(m[1]*m[5] - m[2]*m[4]);
	return std::abs(determinant - 1.0) <= 0.001;
}

// Only these verified Revision A overlay groups use camera-space poses.
// The matrix envelope also guards against a different use of the same mesh.
bool is_camera_space_overlay(const BackgroundObject &object) {
	constexpr std::array<uint32_t, 8> overlay_addresses = {
		0x8d5da4u, 0x8d5efau, 0x8d5f4bu, 0x8d5f9cu,
		0x8d60cdu, 0x8d61bfu, 0x8d65eeu, 0x94dd59u,
	};
	const auto address = object.geometry[0];
	const auto &pose = object.model_view;
	// The rev needle (0x8d6a0f) sits 0.3 in front of the eye and turns with the
	// revs, so its rotation is not tiny: its depth is the guard. Taken as scenery
	// it moved with the camera between ticks and bobbed off its arc (21/11/2026).
	if (address == 0x8d6a0fu) return pose[11] > 0.25 && pose[11] < 0.35;
	return std::binary_search(overlay_addresses.begin(), overlay_addresses.end(), address) &&
	       pose[11] > 0.0 && pose[11] <= 1.51 &&
	       std::all_of(pose.begin(), pose.begin() + 9,
	                   [](double v) { return std::abs(v) <= 0.03; });
}

auto mobile_key_tuple(const MobileKey &key) { return std::array{key.part, key.slot, key.generation}; }
// Hermite steps for the keyed objects, and carrying on past a late tick
// (M2_POSE_HERMITE=0: straight steps, held at the latest snapshot).
bool pose_extrapolate() {
	static const bool on = [] {
		const char *v = std::getenv("M2_POSE_HERMITE");
		return !(v && v[0] == '0');
	}();
	return on;
}

} // namespace

void Geo::track_shaft_route(bool enable) {
	if (enable == track_shaft_route_) return;
	if (!pose_presentation_) discard_pose_history();
	if (enable) shaft_route_ticks_.fill(shaft_route_);
	track_shaft_route_ = enable;
}

void Geo::stamp_pose_scene(SnapshotStamp stamp) {
	if ((!pose_presentation_ && !track_shaft_route_) || stamp.tick != tick_) return;
	current_stamp_ = stamp;
	if (track_shaft_route_) shaft_route_ticks_[stamp.tick % shaft_route_ticks_.size()] = shaft_route_;
	if (camera_lineage_ && camera_lineage_->finish_scene().hold) return;
	if (!previous_stamp_ || previous_stamp_->session_id != stamp.session_id ||
	    previous_stamp_->tick + 1 != stamp.tick || stamp.discontinuity_before ||
	    (mobile_identity_ && (mobile_identity_->reset_on_tick(stamp.tick) ||
	                          mobile_identity_->reset_on_tick(previous_stamp_->tick))))
		return;

	const auto backgrounds = [](const std::vector<SceneObject> &scene) {
		std::vector<BackgroundObject> objects;
		for (const auto &obj : scene) {
			if (!obj.key.part && (obj.geometry.geometry[0] & 0x01800000) == 0x00800000 &&
			    !is_camera_space_overlay(obj.geometry))
				objects.push_back(obj.geometry);
		}
		return objects;
	};
	const auto match = fit_background_motion(backgrounds(previous_scene_), backgrounds(current_scene_));
	// The fitted translation norm is the distance between camera origins
	// for a rigid transform. Normal-race traces stayed below two units.
	constexpr double max_camera_step = 3.0;
	if (match && !match->cut_candidate() && rigid_background_delta(match->transform) &&
	    std::hypot(match->transform[9], match->transform[10], match->transform[11]) <= max_camera_step &&
	    match->support * 2 >= match->common_objects) {
		if (track_shaft_route_) {
			// The fit takes the old view into the new view. The camera
			// advances when the static world moves back in the new view.
			shaft_route_ -= match->transform[11];
			shaft_route_ticks_[stamp.tick % shaft_route_ticks_.size()] = shaft_route_;
		}
		if (pose_presentation_) {
			background_motion_ = match;
			bind_windshield_reflection();
			bind_ram_emissions();
		}
	}
}

void Geo::discard_pose_history() {
	if (camera_lineage_) camera_lineage_->discard_draw_history();
	previous_stamp_.reset();
	current_stamp_.reset();
	background_motion_.reset();
	older_pair_.reset();
}

void Geo::rotate_pose_frame() {
	// Reuse the buffers of the pair about to be dropped: rebuilt from empty
	// every tick, the primitive list (tens of thousands of entries with every
	// opponent at the near model) was regrown and copied, then freed, each
	// tick (a fifth of the busy time in a profile, 22/11).
	std::vector<ReferencePrimitive> spare_primitives;
	std::vector<SceneObject> spare_previous, spare_current, before_scene;
	std::array<double, 12> before_motion{};
	std::optional<uint64_t> before_tick;
	if (older_pair_) {
		spare_primitives = std::move(older_pair_->primitives);
		spare_previous = std::move(older_pair_->before_scene);
		spare_current = std::move(older_pair_->current_scene);
		// Its first tick is the one before the new older pair's: kept for
		// the speed at that pair's first tick (Hermite steps).
		before_scene = std::move(older_pair_->previous_scene);
		before_motion = older_pair_->motion.transform;
		if (older_pair_->current_stamp.session_id == (previous_stamp_ ? previous_stamp_->session_id : 0))
			before_tick = older_pair_->previous_stamp.tick;
	}
	// Keep the finished pair (n-1, n) as the older one before it rotates.
	if (background_motion_ && previous_stamp_ && current_stamp_) {
		auto &older = older_pair_.emplace();
		older.previous_scene = std::move(previous_scene_);
		older.current_scene = std::move(spare_current);
		older.current_scene.assign(current_scene_.begin(), current_scene_.end());
		older.primitives = std::move(reference_primitives_);
		older.previous_stamp = *previous_stamp_;
		older.current_stamp = *current_stamp_;
		older.motion = *background_motion_;
		older.before_scene = std::move(before_scene);
		older.before_motion = before_motion;
		older.before_valid = before_tick && *before_tick + 1 == older.previous_stamp.tick;
	} else {
		older_pair_.reset();
	}
	if (spare_primitives.capacity() > reference_primitives_.capacity()) reference_primitives_.swap(spare_primitives);
	reference_primitives_.clear();
	previous_scene_.swap(current_scene_);
	if (spare_previous.capacity() > current_scene_.capacity()) current_scene_.swap(spare_previous);
	current_scene_.clear();
	previous_stamp_ = current_stamp_;
	current_stamp_.reset();
	background_motion_.reset();
}

void Geo::swap_pose_pair(PosePair &pair) {
	previous_scene_.swap(pair.previous_scene);
	current_scene_.swap(pair.current_scene);
	reference_primitives_.swap(pair.primitives);
	std::swap(*previous_stamp_, pair.previous_stamp);
	std::swap(*current_stamp_, pair.current_stamp);
	std::swap(*background_motion_, pair.motion);
}

const std::vector<GeoPoly> *Geo::present_pose_scene(uint64_t now_ns, uint64_t delay_ns) {
	if (!background_motion_ || !previous_stamp_ || !current_stamp_) return nullptr;
	// Sampled before tick n-1: interpolate n-2 to n-1 (the older pair, which
	// ends on this pair's first tick) rather than holding tick n-1. Held, the
	// cars stood still on 11 to 30 % of the presentations, then jumped (22/11).
	const uint64_t sample_ns = now_ns > delay_ns ? now_ns - delay_ns : 0;
	if (older_pair_ && sample_ns < previous_stamp_->time_ns &&
	    older_pair_->current_stamp.tick == previous_stamp_->tick &&
	    older_pair_->current_stamp.session_id == previous_stamp_->session_id &&
	    !current_stamp_->discontinuity_before) {
		swap_pose_pair(*older_pair_);
		presenting_older_ = true;
		const auto *polys = present_pose_pair(now_ns, delay_ns);
		presenting_older_ = false;
		swap_pose_pair(*older_pair_);
		return polys;
	}
	return present_pose_pair(now_ns, delay_ns);
}

const std::vector<GeoPoly> *Geo::present_pose_pair(uint64_t now_ns, uint64_t delay_ns) {
	auto blend = sample_snapshots(*previous_stamp_, *current_stamp_, now_ns, delay_ns);
	if (!blend.can_interpolate) return nullptr;
	// Past the latest snapshot, draw it through this same replay (alpha 1 is
	// exactly the latest poses) rather than handing back to the game's own
	// display list: in a 40-car race a third of the presentations took
	// that other path, and alternating the two drew the cars differently from
	// one refresh to the next.
	blend.alpha = std::min(blend.alpha, 1.0);
	// A tick late (0.3 to 0.6 % of the presentations at a 1.25 interval
	// delay): held at the latest snapshot, the cars stopped for a refresh,
	// then caught up, a slight vibration (23/11). With Hermite steps,
	// carry on along the last step for up to a quarter interval instead.
	if (blend.alpha >= 1.0 && pose_extrapolate()) {
		const uint64_t sample_ns = now_ns > delay_ns ? now_ns - delay_ns : 0;
		if (sample_ns > current_stamp_->time_ns && current_stamp_->time_ns > previous_stamp_->time_ns) {
			const double span = double(current_stamp_->time_ns - previous_stamp_->time_ns);
			blend.alpha = 1.0 + std::min(double(sample_ns - current_stamp_->time_ns) / span, 0.25);
		}
	}

	using Key = std::array<uint32_t, 3>;
	std::map<Key, const BackgroundObject *> previous_keys;
	struct CurrentPart {
		size_t index{};
		unsigned count{};
	};
	std::map<Key, CurrentPart> current_parts;
	for (const auto &object : previous_scene_) {
		if (!object.key.part || is_camera_space_overlay(object.geometry)) continue;
		auto [it, added] = previous_keys.try_emplace(mobile_key_tuple(object.key), &object.geometry);
		if (!added) it->second = nullptr;
	}
	for (size_t i = 0; i < current_scene_.size(); ++i) {
		const auto &object = current_scene_[i];
		if (object.key.part && !is_camera_space_overlay(object.geometry)) {
			auto &part = current_parts[mobile_key_tuple(object.key)];
			part.index = i;
			++part.count;
		}
	}

	// Previous chassis of each opponent car, whatever its level of detail.
	std::map<std::array<uint32_t, 2>, const BackgroundObject *> previous_chassis_by_car;
	for (const auto &object : previous_scene_) {
		if (!is_opponent_body_part(object.key.part) || is_camera_space_overlay(object.geometry)) continue;
		auto [it, added] = previous_chassis_by_car.try_emplace({object.key.slot, object.key.generation}, &object.geometry);
		if (!added) it->second = nullptr; // two chassis: ambiguous
	}
	auto previous_chassis = [&](const MobileKey &key) -> const BackgroundObject * {
		if (!is_opponent_body_part(key.part)) return nullptr;
		const auto it = previous_chassis_by_car.find({key.slot, key.generation});
		return it == previous_chassis_by_car.end() ? nullptr : it->second;
	};

	const auto &d = background_motion_->transform;
	// Hermite steps: each keyed object's velocity at a tick is its step into
	// that tick, P(k) - P(k-1), fixed once the tick arrives. Two pairs
	// meeting at a tick share it, so a car keeps its speed across ticks (with
	// straight steps it changed speed at every tick: a shake seen at 120 Hz
	// and not in a 60 frames/s recording), and
	// a new tick does not change the curve already being drawn (a central
	// difference did: a slight vibration, 23/11). The tick before this
	// pair's first one: the older pair's first tick when drawing the current
	// pair, the older pair's own before_scene when drawing the older pair.
	// M2_POSE_HERMITE=0: straight steps.
	const bool hermite = pose_extrapolate();
	std::map<Key, Pose> neighbour_poses;
	if (hermite && older_pair_) {
		const bool drawing_older = presenting_older_; // older_pair_ then holds the current pair
		const auto &scene = drawing_older ? older_pair_->before_scene : older_pair_->previous_scene;
		const Pose to_here = drawing_older ? compose(d, older_pair_->before_motion)
		                                   : compose(d, older_pair_->motion.transform);
		const bool continuous = drawing_older ? older_pair_->before_valid
		                                      : older_pair_->current_stamp.tick == previous_stamp_->tick;
		if (continuous)
			for (const auto &object : scene) {
				if (!object.key.part || is_camera_space_overlay(object.geometry)) continue;
				auto [it, added] = neighbour_poses.try_emplace(mobile_key_tuple(object.key),
				                                               compose(to_here, object.geometry.model_view));
				if (!added) it->second[0] = std::numeric_limits<double>::quiet_NaN(); // two: ambiguous
			}
	}
	const auto moving_pose = [&](const Key &key, const Pose &m_prev, const Pose &latest) {
		const Pose start = compose(d, m_prev);
		std::array<double, 3> step, ta, tb;
		for (int i = 0; i < 3; ++i) step[i] = latest[9 + i] - start[9 + i];
		ta = tb = step;
		const auto it = neighbour_poses.find(key);
		if (it == neighbour_poses.end() || std::isnan(it->second[0])) return mobile_pose(d, m_prev, latest, blend.alpha);
		const Pose &before = it->second;
		// A previous step longer than 2 units is a jump: keep straight steps.
		if (std::hypot(start[9] - before[9], start[10] - before[10], start[11] - before[11]) > 2.0)
			return mobile_pose(d, m_prev, latest, blend.alpha);
		for (int i = 0; i < 3; ++i) ta[i] = start[9 + i] - before[9 + i]; // the step into this pair's first tick
		return mobile_pose_hermite(d, m_prev, latest, blend.alpha, ta, tb);
	};
	// The camera on the same Hermite steps. The scenery motion d moves it in
	// straight steps from tick to tick; with the cars smoothed and not the
	// camera, a car moving with the camera (alongside, ahead) jerked on the
	// screen at each tick. Its position, in this
	// pair's latest frame: t_d at the first tick, 0 at the latest, entering
	// with the previous step (the pair before's motion, carried into this
	// frame) and leaving with this one. Every smoothed object moves by the
	// difference from the straight path, seen from the camera's axes.
	std::array<double, 3> camera_shift{};
	if (hermite && older_pair_ && blend.alpha > 0.0 && blend.alpha < 1.0) {
		const bool drawing_older = presenting_older_;
		const bool continuous = drawing_older ? older_pair_->before_valid
		                                      : older_pair_->current_stamp.tick == previous_stamp_->tick;
		const auto &before_motion = drawing_older ? older_pair_->before_motion : older_pair_->motion.transform;
		const std::array<double, 3> td = {d[9], d[10], d[11]};
		std::array<double, 3> v0{}, v1{};
		for (int r = 0; r < 3; ++r) {
			v1[r] = -td[r];
			v0[r] = -(d[r] * before_motion[9] + d[3 + r] * before_motion[10] + d[6 + r] * before_motion[11]);
		}
		if (continuous && std::hypot(v0[0], v0[1], v0[2]) <= 3.0 && std::hypot(v1[0], v1[1], v1[2]) <= 3.0) {
			const double t = blend.alpha, t2 = t * t, t3 = t2 * t;
			const double h10 = t3 - 2 * t2 + t, h11 = t3 - t2;
			std::array<double, 3> delta; // Hermite minus straight (h00 + h01 = 1, h10 + h11 = t - straight's t)
			for (int r = 0; r < 3; ++r) delta[r] = h10 * (v0[r] - v1[r]);
			(void)h11;
			const Pose turn = rigid_power(d, blend.alpha - 1);
			for (int r = 0; r < 3; ++r)
				camera_shift[r] = -(turn[r] * delta[0] + turn[3 + r] * delta[1] + turn[6 + r] * delta[2]);
		}
	}
	sampled_matrices_.resize(current_scene_.size());
	// How each object's pose was chosen (diagnostic M2_POSE_TRACE and the
	// slot-part carry below): 0 latest (overlay, wheel, duplicate key),
	// 1 scenery (no previous key), 2 latest (geometry changed),
	// 3 interpolated, 4 latest (jump), 5 carried with its chassis.
	pose_branch_.assign(current_scene_.size(), 0);
	for (size_t i = 0; i < current_scene_.size(); ++i) {
		const auto &object = current_scene_[i];
		const auto &latest = object.geometry.model_view;
		const auto key = mobile_key_tuple(object.key);
		const auto previous = previous_keys.find(key);
		Pose pose;
		if (is_camera_space_overlay(object.geometry) ||
		    opponent_wheel_chassis_part(object.key.part) ||
		    (object.key.part && current_parts.at(key).count > 1)) {
			pose = latest;
		} else if (const auto *lod = previous == previous_keys.end() ? previous_chassis(object.key) : nullptr) {
			// A car that changed level of detail between ticks (0x93dc, 0x9000,
			// 0x8c00, 0x8808) keeps its slot and generation: interpolate from the
			// previous level's pose. Taken as scenery it was drawn one frame at
			// its latest position, then pulled back: a shaking, doubled car.
			const Pose moved = compose(d, lod->model_view);
			const double displacement = std::hypot(moved[9]-latest[9], moved[10]-latest[10], moved[11]-latest[11]);
			pose = displacement <= 2.0 ? mobile_pose(d, lod->model_view, latest, blend.alpha) : latest;
			pose_branch_[i] = displacement <= 2.0 ? 3 : 4;
		} else if (previous == previous_keys.end()) {
			pose = scenery_pose(d, latest, blend.alpha);
			pose_branch_[i] = 1;
		} else if (!previous->second || previous->second->geometry != object.geometry.geometry) {
			pose = latest;
			pose_branch_[i] = 2;
		} else {
			const Pose moved = compose(d, previous->second->model_view);
			const double displacement = std::hypot(moved[9]-latest[9], moved[10]-latest[10], moved[11]-latest[11]);
			// C6c puts ordinary self-motion below 1.44 units; the observed reset-like jump was 2.817.
			pose = displacement <= 2.0 ? moving_pose(key, previous->second->model_view, latest) : latest;
			pose_branch_[i] = displacement <= 2.0 ? 3 : 4;
		}
		if (pose_branch_[i] == 1 || pose_branch_[i] == 3)
			for (int r = 0; r < 3; ++r) pose[9 + r] += camera_shift[r];
		for (unsigned j = 0; j < pose.size(); ++j) sampled_matrices_[i][j] = float(pose[j]);
	}
	for (size_t i = 0; i < current_scene_.size(); ++i) {
		const auto &wheel = current_scene_[i];
		const auto chassis_part = opponent_wheel_chassis_part(wheel.key.part);
		if (!chassis_part)
			continue;
		// Every wheel emission is carried, even when several share one key: a
		// wheel left at its latest pose would sit apart from an interpolated chassis.
		const auto chassis = current_parts.find(Key{chassis_part, wheel.key.slot, wheel.key.generation});
		if (chassis == current_parts.end() || chassis->second.count != 1)
			continue;
		const size_t chassis_index = chassis->second.index;
		const auto &native_chassis = current_scene_[chassis_index].geometry.model_view;
		if (!rigid_background_delta(native_chassis))
			continue;

		// Carry the current wheel mesh and its relative pose with the sampled chassis.
		Pose sampled_chassis;
		const auto &chassis_matrix = sampled_matrices_[chassis_index];
		std::copy(chassis_matrix.begin(), chassis_matrix.end(), sampled_chassis.begin());
		const auto local_wheel = compose(inverse(native_chassis), wheel.geometry.model_view);
		const auto sampled_wheel = compose(sampled_chassis, local_wheel);
		for (size_t j = 0; j < sampled_wheel.size(); ++j)
			sampled_matrices_[i][j] = float(sampled_wheel[j]);
		pose_branch_[i] = 5;
	}
	// Local play build: every other part of an opponent slot whose own pose
	// is not interpolated (it appeared, changed mesh, jumped or is keyed
	// twice) is carried rigidly with an interpolated chassis of the same
	// slot and generation, as the wheels are; otherwise it would be drawn
	// at the latest tick while the body lags, a doubled car. Off with
	// M2_POSE_CARRY_PARTS=0.
	static const bool carry_parts = [] {
		const char *v = std::getenv("M2_POSE_CARRY_PARTS");
		return !(v && v[0] == '0');
	}();
	if (carry_parts) {
		constexpr uint32_t chassis_parts[] = {0x8808u, 0x8c00u, 0x9000u, 0x93dcu};
		for (size_t i = 0; i < current_scene_.size(); ++i) {
			const auto &part = current_scene_[i];
			// Every other part of a car follows its chassis, interpolated or not
			// (a part interpolated on its own while the chassis jumped to its
			// latest pose would be drawn apart: a doubled car after a crash).
			if (!part.key.part || pose_branch_[i] == 5 || is_camera_space_overlay(part.geometry))
				continue;
			if (std::find(std::begin(chassis_parts), std::end(chassis_parts), part.key.part) != std::end(chassis_parts))
				continue;
			for (const uint32_t cp : chassis_parts) {
				const auto chassis = current_parts.find(Key{cp, part.key.slot, part.key.generation});
				if (chassis == current_parts.end() || chassis->second.count != 1) continue;
				const size_t ci = chassis->second.index;
				// Whatever pose the chassis got (interpolated, or its latest one
				// after a jump such as a crash shake), its parts follow it.
				const auto &native_chassis = current_scene_[ci].geometry.model_view;
				if (!rigid_background_delta(native_chassis)) continue;
				Pose sampled_chassis;
				std::copy(sampled_matrices_[ci].begin(), sampled_matrices_[ci].end(), sampled_chassis.begin());
				const auto carried = compose(sampled_chassis, compose(inverse(native_chassis), part.geometry.model_view));
				for (size_t j = 0; j < carried.size(); ++j) sampled_matrices_[i][j] = float(carried[j]);
				pose_branch_[i] = 5;
				break;
			}
		}
		// Unkeyed objects drawn at an interpolated chassis' exact position
		// (measured on Expert: oba 0x91 with every near body 0x8808 and 0x10a
		// with every mid body 0x8c00, at distance 0) belong to that car; as
		// scenery they would stay behind while the body moves on: a doubled
		// car. Carry them with that chassis.
		// Each model's chassis sorted by x, with their order in current_parts:
		// every unkeyed object used to look through all the parts of all the
		// cars at each presentation (40 cars at the near model: millions of
		// tests a pass, 22/11). Same choice: the first chassis in that order.
		struct ChassisAt { double x; size_t order, index; };
		std::array<std::vector<ChassisAt>, std::size(chassis_parts)> chassis_by_x;
		{
			size_t order = 0;
			for (const auto &[key, info] : current_parts) {
				++order;
				if (info.count != 1) continue;
				const auto cp = std::find(std::begin(chassis_parts), std::end(chassis_parts), key[0]);
				if (cp == std::end(chassis_parts)) continue;
				chassis_by_x[size_t(cp - std::begin(chassis_parts))].push_back(
				    {current_scene_[info.index].geometry.model_view[9], order, info.index});
			}
			for (auto &list : chassis_by_x)
				std::sort(list.begin(), list.end(), [](const ChassisAt &l, const ChassisAt &r) { return l.x < r.x; });
		}
		for (size_t i = 0; i < current_scene_.size(); ++i) {
			const auto &object = current_scene_[i];
			if (object.key.part || pose_branch_[i] == 3 || pose_branch_[i] == 5 ||
			    is_camera_space_overlay(object.geometry))
				continue;
			const auto &mv = object.geometry.model_view;
			for (const auto &list : chassis_by_x) {
				const ChassisAt *found = nullptr;
				for (auto it = std::lower_bound(list.begin(), list.end(), mv[9] - 1e-4,
				                                [](const ChassisAt &c, double x) { return c.x < x; });
				     it != list.end() && it->x <= mv[9] + 1e-4; ++it) {
					// Any chassis pose: after a jump (crash shake) the chassis is
					// drawn at its latest pose and the stuck-on objects must be too.
					const auto &native_chassis = current_scene_[it->index].geometry.model_view;
					if (std::abs(native_chassis[9] - mv[9]) > 1e-4 || std::abs(native_chassis[10] - mv[10]) > 1e-4 ||
					    std::abs(native_chassis[11] - mv[11]) > 1e-4 || !rigid_background_delta(native_chassis))
						continue;
					if (!found || it->order < found->order) found = &*it;
				}
				if (!found) continue;
				const auto &native_chassis = current_scene_[found->index].geometry.model_view;
				Pose sampled_chassis;
				std::copy(sampled_matrices_[found->index].begin(), sampled_matrices_[found->index].end(),
				          sampled_chassis.begin());
				const auto carried = compose(sampled_chassis, compose(inverse(native_chassis), mv));
				for (size_t j = 0; j < carried.size(); ++j) sampled_matrices_[i][j] = float(carried[j]);
				pose_branch_[i] = 5;
				break;
			}
		}
	}
	static std::FILE *pose_trace = [] {
		const char *path = std::getenv("M2_POSE_TRACE");
		return path && *path ? std::fopen(path, "w") : nullptr;
	}();
	if (pose_trace) {
		static unsigned long presentation = 0;
		// M2_POSE_TRACE_CARS=1: the keyed objects only (the opponents' parts),
		// light enough for a real race (diagnostic, 23/11).
		static const bool cars_only = [] { const char *v = std::getenv("M2_POSE_TRACE_CARS"); return v && v[0] == '1'; }();
		for (size_t i = 0; i < current_scene_.size(); ++i) {
			const auto &o = current_scene_[i];
			if (cars_only && (!o.key.part || o.key.slot == 0)) continue;
			const auto &m = o.geometry.model_view;
			std::fprintf(pose_trace, "%lu,%.3f,%x,%u,%u,%u,%x,%.2f,%.2f,%.2f", presentation,
			             presenting_older_ ? blend.alpha - 1 : blend.alpha, o.key.part,
			             o.key.slot, o.key.generation, unsigned(pose_branch_[i]), o.geometry.geometry[0], m[9], m[10], m[11]);
			static const bool full = [] { const char *v = std::getenv("M2_POSE_TRACE_FULL"); return v && v[0] == '1'; }();
			if (full)
				for (unsigned j = 0; j < 9; ++j) std::fprintf(pose_trace, ",%.5f", m[j]);
			// The pose actually drawn this presentation (sampled), and the time
			// it was sampled for, in ns: the columns that show a doubled car.
			static const bool drawn = [] { const char *v = std::getenv("M2_POSE_TRACE_DRAWN"); return v && v[0] == '1'; }();
			if (drawn)
				std::fprintf(pose_trace, ",%.3f,%.3f,%.3f,%llu", sampled_matrices_[i][9], sampled_matrices_[i][10],
				             sampled_matrices_[i][11], (unsigned long long)(now_ns > delay_ns ? now_ns - delay_ns : 0));
			std::fputc('\n', pose_trace);
		}
		++presentation;
		if (presentation % 120 == 0) std::fflush(pose_trace);
	}
	if (track_shaft_route_)
		presented_shaft_route_ = std::lerp(shaft_route_ticks_[previous_stamp_->tick % shaft_route_ticks_.size()],
		                                  shaft_route_ticks_[current_stamp_->tick % shaft_route_ticks_.size()],
		                                  blend.alpha);
	presented_casters_.clear();
	if (top_margin_ && behind_casters_on()) replay_casters_ = &presented_casters_;
	replay_instance_reference(presented_polys_, true, sampled_matrices_);
	replay_casters_ = nullptr;
	return &presented_polys_;
}

/* 3D Rasterizer frame start: Resets frame variables */
void Geo::render_frame_start()
{
	raster_state *raster = &raster_;

	/* reset the polygon list index */
	raster->poly_list_index = 0;
	polys.clear();
	shadow_casters_.clear();
	cur_object_command = 0;
	capturing_rom_object_ = false;
	pushed.clear();

	/* reset the min-max sortable Z values */
	raster->min_z = 0xffff;
	raster->max_z = 0;

	/* reset the polygon z value */
	// Zero Gunner sets backgrounds with "previous z value" mode at the start of the display list,
	// needs this to be this big in order to work properly
	raster->polygon_z = 1e10;

	raster->cur_window = 0;
}

/* 3D Rasterizer main data input port */
void Geo::model2_3d_push(raster_state *raster, uint32_t input)
{
	if (record_pushes) pushed.push_back(input);
	/* see if we have a command in progress */
	if (raster->cur_command != 0)
	{
		raster->command_buffer[raster->command_index++] = input;

		switch (raster->cur_command)
		{
			case 0x00:  /* NOP */
			break;

			case 0x01:  /* Polygon Data */
			{
				uint32_t  attr;

				/* start by looking if we have the basic input data */
				if (raster->command_index < 9)
					return;

				/* get the attributes */
				attr = raster->command_buffer[8];

				/* see if we're done */
				if ((attr & 3) == 0)
				{
					raster->cur_command = 0;
					return;
				}

				/* see if it's a quad or a triangle */
				if (attr & 1)
				{
					/* it's a quad, wait for the rest of the points */
					if (raster->command_index < 17)
						return;

					/* we have a full polygon info, fill up our polygon structure */
					model2_3d_process_polygon<4>(raster, attr);

					/* back up and wait for more data */
					raster->command_index = 8;
				}
				else
				{
					/* it's a triangle, wait for the rest of the point */
					if (raster->command_index < 14)
						return;

					/* we have a full polygon info, fill up our polygon structure */
					model2_3d_process_polygon<3>(raster, attr);

					/* back up and wait for more data */
					raster->command_index = 8;
				}
			}
			break;

			case 0x03:  /* Window Data */
			{
				uint32_t  i;

				/* make sure we have all the data */
				if (raster->command_index < 6)
					return;

				/* coordinates are 12 bit signed */

				/* extract the viewport start x */
				raster->viewport[0] = (raster->command_buffer[0] >> 12) & 0xfff;

				if (raster->viewport[0] & 0x800)
					raster->viewport[0] = -(0x800 - (raster->viewport[0] & 0x7ff));

				/* extract the viewport start y */
				raster->viewport[1] = raster->command_buffer[0] & 0xfff;

				if (raster->viewport[1] & 0x800)
					raster->viewport[1] = -(0x800 - (raster->viewport[1] & 0x7ff));

				/* extract the viewport end x */
				raster->viewport[2] = (raster->command_buffer[1] >> 12) & 0xfff;

				if (raster->viewport[2] & 0x800)
					raster->viewport[2] = -(0x800 - (raster->viewport[2] & 0x7ff));

				/* extract the viewport end y */
				raster->viewport[3] = raster->command_buffer[1] & 0xfff;

				if (raster->viewport[3] & 0x800)
					raster->viewport[3] = -(0x800 - (raster->viewport[3] & 0x7ff));

				/* extract the centers */
				for (i = 0; i < 4; i++)
				{
					/* center x */
					raster->center[i][0] = (raster->command_buffer[2+i] >> 12) & 0xfff;

					if (raster->center[i][0] & 0x800)
						raster->center[i][0] = -(0x800 - (raster->center[i][0] & 0x7ff));

					/* center y */
					raster->center[i][1] = raster->command_buffer[2+i] & 0xfff;

					if (raster->center[i][1] & 0x800)
						raster->center[i][1] = -(0x800 - (raster->center[i][1] & 0x7ff));

					// calculate clipping planes
					float left_plane = float(raster->center[i][0] - raster->viewport[0]);
					float right_plane = float(raster->viewport[2] - raster->center[i][0]);
					// Widescreen: a viewport spanning the screen opens out at both sides.
					if (wide_margin_ && raster->viewport[0] <= 0 && raster->viewport[2] >= 495)
					{
						left_plane += float(wide_margin_);
						right_plane += float(wide_margin_);
					}
					float top_plane = float(raster->viewport[3] - raster->center[i][1]);
					// Ray-traced shadows: what is just above or beside a
					// full-screen viewport (gantry and arch overhead, poles
					// and panels at the sides) is kept for the shadows; the
					// renderer's clip rectangle still hides it.
					if (top_margin_ && raster->viewport[0] <= 0 && raster->viewport[2] >= 495)
					{
						top_plane += float(top_margin_);
						left_plane += float(top_margin_);
						right_plane += float(top_margin_);
					}
					float bottom_plane = float(raster->center[i][1] - raster->viewport[1]);

					raster->clip_plane[i][0].normal.x = 1.0f / std::hypot(1.0f, left_plane);
					raster->clip_plane[i][0].normal.y = 0.0f;
					raster->clip_plane[i][0].normal.pz = left_plane / std::hypot(1.0f, left_plane);

					raster->clip_plane[i][1].normal.x = -1.0f / std::hypot(-1.0f, right_plane);
					raster->clip_plane[i][1].normal.y = 0.0f;
					raster->clip_plane[i][1].normal.pz = right_plane / std::hypot(-1.0f, right_plane);

					raster->clip_plane[i][2].normal.x = 0.0f;
					raster->clip_plane[i][2].normal.y = -1.0f / std::hypot(-1.0f, top_plane);
					raster->clip_plane[i][2].normal.pz = top_plane / std::hypot(-1.0f, top_plane);

					raster->clip_plane[i][3].normal.x = 0.0f;
					raster->clip_plane[i][3].normal.y = 1.0f / std::hypot(1.0f, bottom_plane);
					raster->clip_plane[i][3].normal.pz = bottom_plane / std::hypot(1.0f, bottom_plane);
				}

				/* done with this command */
				raster->cur_command = 0;
			}
			break;

			case 0x04:  /* Texture/Log Data write */
			{
				/* make sure we have enough data */
				if (raster->command_index < 2)
					return;

				/* see if the count is non-zero */
				if (raster->command_buffer[1] > 0)
				{
					/* see if we have data available */
					if (raster->command_index >= 3)
					{
						/* get the address */
						uint32_t  address = raster->command_buffer[0];

						/* do the write */
						if (address & 0x800000)
							raster->texture_ram[address & 0xffff] = raster->command_buffer[2];
						else
							raster->log_ram[address & 0x7fff] = raster->command_buffer[2];

						/* increment the address and decrease the count */
						raster->command_buffer[0]++;
						raster->command_buffer[1]--;

						/* decrease the index, so we keep placing data in the same slot */
						raster->command_index--;
					}
				}

				/* see if we're done with this command */
				if (raster->command_buffer[1] == 0)
					raster->cur_command = 0;
			}
			break;

			case 0x08:  /* ZSort mode */
			{
				/* save the zsort mode value */
				raster->z_adjust = raster->command_buffer[0] << 8;

				/* done with this command */
				raster->cur_command = 0;
			}
			break;

			default:
			{
				throw GeoFatal("SEGA 3D: Unknown rasterizer command");
			}
		}
	}
	else
	{
		/* new command */
		raster->cur_command = input & 0x0f;
		raster->command_index = 0;

		/* see if it's object data */
		if (raster->cur_command == 1)
		{
			/* extract reverse bit */
			raster->reverse = (input >> 4) & 1;

			/* extract center select */
			raster->center_sel = (input >> 6) & 3;
		}
	}
}

/***********************************************************************************************/


inline void Geo::model2_3d_push_point(raster_state *raster, const GeoVertex &point)
{
    const uint32_t x = f2u(point.x) >> 8;
    const uint32_t y = f2u(point.y) >> 8;
    const uint32_t z = f2u(point.pz) >> 8;
    const uint32_t index = raster->command_index;
    if (index < 32) {
        raster->object_pos[index][0] = point.ox;
        raster->object_pos[index][1] = point.oy;
        raster->object_pos[index][2] = point.oz;
    }
    // These slots contain a whole point. Only its final word can complete a
    // polygon, so the X/Y words need no repeated command-decoder dispatch.
    if (raster->cur_command == 1 &&
        (index == 2 || index == 5 || index == 11 ||
         (index == 14 && (raster->command_buffer[8] & 1))))
    {
        if (record_pushes) { pushed.push_back(x); pushed.push_back(y); }
        raster->command_buffer[index] = x;
        raster->command_buffer[index + 1] = y;
        raster->command_index = index + 2;
    }
    else
    {
        model2_3d_push(raster, x);
        model2_3d_push(raster, y);
    }
    model2_3d_push(raster, z);
}

/* Parse Polygons: Normals Present, No Specular case */
void Geo::geo_parse_np_ns(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	M2_GEO_LOAD_MATRIX(geo); // (the matrix is the same for the whole object)
	GeoVertex point, normal;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	M2_GEO_POINT(&point, geo);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	M2_GEO_POINT(&point, geo);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		/* read in the normal */
		normal.x = u2f(*input++);
		normal.y = u2f(*input++);
		normal.pz = u2f(*input++);
		if (capturing_rom_object_) {
			rom_link_attributes_ = attr;
			rom_link_normal_ = {normal.x, normal.y, normal.pz};
		}

		/* transform with the current matrix */
		M2_GEO_VECTOR(&normal, geo);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance;
			float               coef, face;
			int32_t               luma;
			texture_parameter * texparam;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			M2_GEO_POINT(&point, geo);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			luminance = (luminance * texparam->diffuse) + texparam->ambient;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push_point(raster, point);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				M2_GEO_POINT(&point, geo);

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push_point(raster, point);
			}
			else /* triangle */
			{
				/* skip the next 3 points */
				input += 3;
			}
		}
		else /* we're done */
		{
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: Normals Present, Specular case */
void Geo::geo_parse_np_s(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal;
	M2_GEO_LOAD_MATRIX(geo); // (the matrix is the same for the whole object)
	uint32_t  attr, i;

	/* read the 1st point */
	read_point(input, point);

	/* transform with the current matrix */
	M2_GEO_POINT(&point, geo);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* read the 2nd point */
	read_point(input, point);

	/* transform with the current matrix */
	M2_GEO_POINT(&point, geo);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

#ifdef M2_FAST_GEO
		// The rasterizer waiting for this polygon's attribute (polygon data,
		// slot 8): this polygon's words go straight into its slots and
		// model2_3d_process_polygon is called directly, as model2_3d_push
		// would do word by word (the same slots, the same calls, in order).
		const bool direct = !record_pushes && raster->cur_command == 1 && raster->command_index == 8;
		if (direct)
		{
			raster->command_buffer[8] = attr & 0x0003ffff;
			raster->command_index = 9;
			if ((attr & 3) == 0) raster->cur_command = 0;
		}
		else
#endif
		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		/* read in the normal */
		read_point(input, normal);
		if (capturing_rom_object_) {
			rom_link_attributes_ = attr;
			rom_link_normal_ = {normal.x, normal.y, normal.pz};
		}

		/* transform with the current matrix */
		M2_GEO_VECTOR(&normal, geo);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance, specular;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* read in the next point */
			read_point(input, point);

			/* transform with the current matrix */
			M2_GEO_POINT(&point, geo);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

#ifdef M2_FAST_GEO
			// A polygon model2_3d_process_polygon will cull whatever its light
			// (check_culling's first two tests: single-sided and facing away,
			// or link type 0) needs no lighting: its luma word only the face
			// bit, its LOD word nothing (neither is read for a culled one).
			if (direct && !recalculate_facing_ && ((((attr >> 17) & 1) == 0 && face != 0) || ((attr >> 8) & 3) == 0))
			{
				luma = int32_t(face);
				distance = 0;
			}
			else
#endif
			{
			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance and specular */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			specular = ((2*dotl) * normal.pz) - geo->light.pz;
			if (specular < 0) specular = 0;
			if (texparam->specular_control == 0) specular = 0;
			if ((texparam->specular_control >> 1) != 0) specular *= specular;
			if ((texparam->specular_control >> 2) != 0) specular *= specular;
			if (((texparam->specular_control+1) >> 3) != 0) specular *= specular;

			specular *= texparam->specular_scale;

			luminance = (luminance * texparam->diffuse) + texparam->ambient + specular;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;
			}

#ifdef M2_FAST_GEO
			if (direct)
			{
				uint32_t *const cb = raster->command_buffer;
				cb[9] = uint32_t(luma << 15);
				cb[10] = f2u(distance) >> 8;
				cb[11] = f2u(point.x) >> 8;
				cb[12] = f2u(point.y) >> 8;
				cb[13] = f2u(point.pz) >> 8;
				raster->object_pos[11][0] = point.ox;
				raster->object_pos[11][1] = point.oy;
				raster->object_pos[11][2] = point.oz;
				if (attr & 1)
				{
					read_point(input, point);
					M2_GEO_POINT(&point, geo);
					apply_focus(geo, &point);
					cb[14] = f2u(point.x) >> 8;
					cb[15] = f2u(point.y) >> 8;
					cb[16] = f2u(point.pz) >> 8;
					raster->object_pos[14][0] = point.ox;
					raster->object_pos[14][1] = point.oy;
					raster->object_pos[14][2] = point.oz;
					raster->command_index = 17;
					model2_3d_process_polygon<4>(raster, cb[8]);
				}
				else
				{
					raster->command_index = 14;
					model2_3d_process_polygon<3>(raster, cb[8]);
					input += 3; /* skip the next 3 points */
				}
				raster->command_index = 8;
				continue;
			}
#endif
			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push_point(raster, point);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				read_point(input, point);

				/* transform with the current matrix */
				M2_GEO_POINT(&point, geo);

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push_point(raster, point);
			}
			else /* triangle */
			{
				/* skip the next 3 points */
				input += 3;
			}
		}
		else /* we're done */
		{
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: No Normals, No Specular case */
void Geo::geo_parse_nn_ns(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	M2_GEO_LOAD_MATRIX(geo); // (the matrix is the same for the whole object)
	GeoVertex point, normal, p0, p1, p2, p3;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	M2_GEO_POINT(&point, geo);

	/* save for normal calculation */
	p0.x = point.x; p0.y = point.y; p0.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	M2_GEO_POINT(&point, geo);

	/* save for normal calculation */
	p1.x = point.x; p1.y = point.y; p1.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* Skip normal */
			if (capturing_rom_object_) rom_link_attributes_ = attr;
			input += 3;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			M2_GEO_POINT(&point, geo);

			/* save for normal calculation */
			p2.x = point.x; p2.y = point.y; p2.pz = point.pz;

			/* compute the normal */
			vector_cross3(&normal, &p0, &p1, &p2);

			/* normalize it */
			normalize_vector(&normal);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			luminance = (luminance * texparam->diffuse) + texparam->ambient;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push_point(raster, point);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				M2_GEO_POINT(&point, geo);

				/* save for normal calculation */
				p3.x = point.x; p3.y = point.y; p3.pz = point.pz;

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push_point(raster, point);
			}
			else
			{
				/* skip the next 3 points */
				input += 3;

				/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
				p3.x = p2.x; p3.y = p2.y; p3.pz = p2.pz;
			}
		}
		else /* we're done */
		{
			break;
		}

		/* link type */
		switch ((attr>>8) & 3)
		{
			case 0:
			case 2:
			{
				/* reuse P0(n) and P1(n) */
				p0.x = p2.x; p0.y = p2.y; p0.pz = p2.pz;
				p1.x = p3.x; p1.y = p3.y; p1.pz = p3.pz;
			}
			break;

			case 1:
			{
				/* reuse P0(n-1) and P0(n) */
				p1.x = p2.x; p1.y = p2.y; p1.pz = p2.pz;
			}
			break;

			case 3:
			{
				/* reuse P1(n-1) and P1(n) */
				p0.x = p3.x; p0.y = p3.y; p0.pz = p3.pz;
			}
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: No Normals, Specular case */
void Geo::geo_parse_nn_s(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	M2_GEO_LOAD_MATRIX(geo); // (the matrix is the same for the whole object)
	GeoVertex point, normal, p0, p1, p2, p3;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	M2_GEO_POINT(&point, geo);

	/* save for normal calculation */
	p0.x = point.x; p0.y = point.y; p0.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	M2_GEO_POINT(&point, geo);

	/* save for normal calculation */
	p1.x = point.x; p1.y = point.y; p1.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance, specular;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* Skip normal */
			if (capturing_rom_object_) rom_link_attributes_ = attr;
			input += 3;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			M2_GEO_POINT(&point, geo);

			/* save for normal calculation */
			p2.x = point.x; p2.y = point.y; p2.pz = point.pz;

			/* compute the normal */
			vector_cross3(&normal, &p0, &p1, &p2);

			/* normalize it */
			normalize_vector(&normal);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance and specular */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			specular = ((2*dotl) * normal.pz) - geo->light.pz;
			if (specular < 0) specular = 0;
			if (texparam->specular_control == 0) specular = 0;
			if ((texparam->specular_control >> 1) != 0) specular *= specular;
			if ((texparam->specular_control >> 2) != 0) specular *= specular;
			if (((texparam->specular_control+1) >> 3) != 0) specular *= specular;

			specular *= texparam->specular_scale;

			luminance = (luminance * texparam->diffuse) + texparam->ambient + specular;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push_point(raster, point);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				M2_GEO_POINT(&point, geo);

				/* save for normal calculation */
				p3.x = point.x; p3.y = point.y; p3.pz = point.pz;

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push_point(raster, point);
			}
			else
			{
				/* skip the next 3 points */
				input += 3;

				/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
				p3.x = p2.x; p3.y = p2.y; p3.pz = p2.pz;
			}
		}
		else /* we're done */
		{
			break;
		}

		/* link type */
		switch ((attr>>8) & 3)
		{
			case 0:
			case 2:
			{
				/* reuse P0(n) and P1(n) */
				p0.x = p2.x; p0.y = p2.y; p0.pz = p2.pz;
				p1.x = p3.x; p1.y = p3.y; p1.pz = p3.pz;
			}
			break;

			case 1:
			{
				/* reuse P0(n-1) and P0(n) */
				p1.x = p2.x; p1.y = p2.y; p1.pz = p2.pz;
			}
			break;

			case 3:
			{
				/* reuse P1(n-1) and P1(n) */
				p0.x = p3.x; p0.y = p3.y; p0.pz = p3.pz;
			}
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/*******************************************
 *
 *  Geometry Engine Commands
 *
 *******************************************/

/* Command 00: NOP */
GeoPtr Geo::geo_nop(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* push the opcode to the 3d rasterizer */
	model2_3d_push(raster, opcode >> 23);

	return input;
}

/* Command 01: Object Data */
GeoPtr Geo::geo_object_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	++cur_object_command;
    if (overlay_roles_.active() && write_provenance_ && opcode == 0x20800000u) {
        const uint32_t word = input.i - 1;
        const auto &header = write_provenance_->at(word);
        if (header.geometry_port == 0xffffffffu)
            overlay_roles_.consume(word, cur_object_command, header.writes);
    }
	raster_state *raster = geo->raster;
	uint32_t  tpa = *input++;     /* Texture Point Address */
	uint32_t  tha = *input++;     /* Texture Header Address */
	uint32_t  oba = *input++;     /* Object Address */
	uint32_t  obc = *input++;     /* Object Count */

	GeoPtr obp;                /* Object Pointer */

	/* push the initial set of data to the 3d rasterizer */
	model2_3d_push(raster, opcode >> 23);
	model2_3d_push(raster, tpa);
	model2_3d_push(raster, tha);

#ifdef M2_DC_SPEED
	// Not shown: no polygons. The parsers change no state of their own; the
	// rasterizer ends the command as after a parse (the two initial points,
	// then attribute 0). What it leaves in its command buffer is written
	// again before it is next read.
	if (skip_objects)
	{
		for (int i = 0; i < 7; i++)
			model2_3d_push(raster, 0);
		return input;
	}
#endif

	/* select where we're reading polygon information from */
	if (oba & 0x01000000)
	{
		/* Fast polygon RAM */
		obp = GeoPtr{geo->polygon_ram1, 0x8000, oba & 0x7fff};
	}
	else if (oba & 0x00800000)
	{
		/* Polygon ROM */
#ifdef M2_DC_MEMORY
		obp = GeoPtr{geo->polygon_rom, geo->polygon_rom_mask + 1, oba & geo->polygon_rom_mask, geo->rom};
#else
		obp = GeoPtr{geo->polygon_rom, geo->polygon_rom_mask + 1, oba & geo->polygon_rom_mask};
#endif
	}
	else
	{
		/* Slow Polygon RAM */
		obp = GeoPtr{geo->polygon_ram0, 0x8000, oba & 0x7fff};
	}

	// if count == 0 then rolls over to max size
	// Virtual On & Gunblade NY
	if (obc == 0)
		obc = 0xfffff;

	m2trace::begin_object(oba, obc, geo->mode & 3, geo->matrix, tpa, tha);
	cur_oba = oba; cur_obc = obc; cur_mode = uint8_t(geo->mode & 3); cur_direct = 0;
	current_mobile_key_ = mobile_identity_ && (oba & 0x01800000) == 0x00800000
		? mobile_identity_->read(command_word_, opcode, tick_) : MobileKey{};
	if (mobile_key_trace_ && current_mobile_key_.part &&
		std::fprintf(mobile_key_trace_, "%llu,%u,%u,%u,%u\n", (unsigned long long)tick_,
			command_word_, current_mobile_key_.part, current_mobile_key_.slot,
			current_mobile_key_.generation) < 0)
		throw GeoFatal("mobile key trace write failed");
	std::memcpy(cur_matrix, geo->matrix, sizeof cur_matrix);
	cur_focus_x = geo->focus.x; cur_focus_y = geo->focus.y;

	if (pose_presentation_ || (track_shaft_route_ && !current_mobile_key_.part &&
	                           (oba & 0x01800000) == 0x00800000)) {
		auto &scene_object = current_scene_.emplace_back();
		scene_object.geometry.geometry = {oba, obc, cur_mode};
		std::copy_n(geo->matrix, 12, scene_object.geometry.model_view.begin());
		scene_object.key = current_mobile_key_;
		// The mid-detail wheel can be the sole draw consuming a verified world focus.
		const bool observes_world_focus = !scene_object.key.part || scene_object.key.part == opponent_wheel_mid;
		if (camera_lineage_ && observes_world_focus &&
		    (oba & 0x01800000)==0x00800000 && !is_camera_space_overlay(scene_object.geometry))
			camera_lineage_->observe_world_object();
	}
	capturing_rom_object_ = (rom_object_sink_ || instance_reference_ || pose_presentation_) &&
		(oba & 0x01800000) == 0x00800000;
	if (capturing_rom_object_) {
		rom_object_.oba = oba;
		rom_object_.obc = obc;
		rom_object_.opcode = opcode;
		rom_object_.command = cur_object_command;
		rom_object_.command_word = command_word_;
		rom_object_.tpa = tpa;
		rom_object_.tha = tha;
		rom_object_.mode = cur_mode;
		std::copy_n(geo->matrix, 12, rom_object_.matrix.begin());
		rom_object_.focus_x = cur_focus_x;
		rom_object_.focus_y = cur_focus_y;
		rom_object_.primitives.clear();
	}

	switch (geo->mode & 3)
	{
		/* Normals present, No Specular */
		case 0: geo_parse_np_ns(geo, obp, obc); break;

		/* Normals present, Specular */
		case 1: geo_parse_np_s(geo, obp, obc); break;

		/* No Normals present, No Specular */
		case 2: geo_parse_nn_ns(geo, obp, obc); break;

		/* No Normals present, Specular */
		case 3: geo_parse_nn_s(geo, obp, obc); break;
	}
	if (capturing_rom_object_) {
		capturing_rom_object_ = false;
		if (rom_object_sink_) rom_object_sink_(rom_object_context_, rom_object_);
	}

	/* move by 4 parameters */
	return input;
}

/* Command 02: Direct Data */
GeoPtr Geo::geo_direct_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;
	uint32_t  tpa = *input++;     /* Texture Point Address */
	uint32_t  tha = *input++;     /* Texture Header Address */
	m2trace::begin_direct(tpa, tha);
	cur_oba = 0; cur_obc = 0; cur_mode = 0; cur_direct = 1;
	current_mobile_key_ = {};
	std::memset(cur_matrix, 0, sizeof cur_matrix);
	cur_focus_x = geo->focus.x; cur_focus_y = geo->focus.y;

	/* push the initial set of data to the 3d rasterizer */
	model2_3d_push(raster, (opcode >> 23) - 1);
	model2_3d_push(raster, tpa);
	model2_3d_push(raster, tha);

	/* push the initial points */
	model2_3d_push(raster, (*input++) >> 8); /* x */
	model2_3d_push(raster, (*input++) >> 8); /* y */
	model2_3d_push(raster, (*input++) >> 8); /* z */

	model2_3d_push(raster, (*input++) >> 8); /* x */
	model2_3d_push(raster, (*input++) >> 8); /* y */
	model2_3d_push(raster, (*input++) >> 8); /* z */

	/* read in the attributes */
	uint32_t  attr;
	while (((attr = *input++) & 3) != 0)
	{
		/* push attributes */
		model2_3d_push(raster, attr & 0x00ffffff);

		/* push luma */
		model2_3d_push(raster, (*input++) >> 8);

		/* push distance */
		model2_3d_push(raster, (*input++) >> 8);

		/* push the next point */
		model2_3d_push(raster, (*input++) >> 8); /* x */
		model2_3d_push(raster, (*input++) >> 8); /* y */
		model2_3d_push(raster, (*input++) >> 8); /* z */

		/* if it's a quad, output another point */
		if (attr & 1)
		{
			model2_3d_push(raster, (*input++) >> 8); /* x */
			model2_3d_push(raster, (*input++) >> 8); /* y */
			model2_3d_push(raster, (*input++) >> 8); /* z */
		}
	}

	/* we're done */
	model2_3d_push(raster, 0);

	return input;
}

/* Command 03: Window Data */
GeoPtr Geo::geo_window_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	raster->cur_window++;

	/*
	    we're going to move 6 coordinates to the 3d rasterizer:
	    - starting coordinate
	    - completion coordinate
	    - vanishing point 0 (eye mode 0)
	    - vanishing point 1 (eye mode 1)
	    - vanishing point 2 (eye mode 2)
	    - vanishing point 3 (eye mode 3)
	*/

	for (uint32_t i = 0; i < 6; i++)
	{
		/* read in the coordinate */
		uint32_t y = *input++;

		/* convert to the 3d rasterizer format (00XXXYYY) */
		uint32_t x = (y & 0x0fff0000) >> 4 ;
		y &= 0xfff;

		/* push it */
		model2_3d_push(raster, x | y);
	}

	return input;
}

/* Command 04: Texture Data Write */
GeoPtr Geo::geo_texture_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the starting address/dsp id */
	model2_3d_push(raster, *input++);

	/* get the count */
	uint32_t count = *input++;

	/* push the count */
	model2_3d_push(raster, count);

	/* loop and send the data */
	for (uint32_t i = 0; i < count; i++)
		model2_3d_push(raster, *input++);

	return input;
}

/* Command 05: Polygon Data */
GeoPtr Geo::geo_polygon_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  address, count, i;
	GeoPtr p;

	(void)opcode;

	/* read in the address */
	address = *input++;

	/* prepare the pointer */
	if (address & 0x01000000)
	{
		/* Fast polygon RAM */
		p = GeoPtr{geo->polygon_ram1, 0x8000, address & 0x7fff};
	}
	else
	{
		/* Slow Polygon RAM */
		p = GeoPtr{geo->polygon_ram0, 0x8000, address & 0x7fff};
	}

	/* read the count */
	count = *input++;

	/* move the data */
	for (i = 0; i < count; i++)
		*p++ = *input++;

	return input;
}

/* Command 06: Texture Parameters */
GeoPtr Geo::geo_texture_parameters(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  index, count, i, param;

	(void)opcode;

	/* read in the index */
	index = (*input++) >> 2;

	/* read in the conut */
	count = *input++;

	for (i = 0; i < count; i++)
	{
		/* read in the texture parameters */
		param = *input++;

		geo->texture_parameters[index].diffuse = float(param & 0xff);
		geo->texture_parameters[index].ambient = float((param >> 8) & 0xff);
		geo->texture_parameters[index].specular_control = (param >> 24) & 0xff;
		geo->texture_parameters[index].specular_scale = float((param >> 16) & 0xff);

		/* read in the distance coefficient */
		geo->coef_table[index] = u2f(*input++);

		index = (index + 1) & 0x1f;
	}

	return input;
}

/* Command 07: Geo Mode */
GeoPtr Geo::geo_mode(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read in the mode */
	geo->mode = *input++;

	return input;
}

/* Command 08: ZSort Mode */
GeoPtr Geo::geo_zsort_mode(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* push the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the mode */
	model2_3d_push(raster, (*input++) >> 8);

	return input;
}

/* Command 09: Focal Distance */
GeoPtr Geo::geo_focal_distance(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	const uint32_t word = input.i;
	const uint32_t x = *input++, y = *input++;
	geo->focus.x = u2f(x);
	geo->focus.y = u2f(y);
	if (camera_lineage_) camera_lineage_->observe_focus(word, x, y, camera_write_provenance_);
	return input;
}

/* Command 0A: Light Source Vector Write */
GeoPtr Geo::geo_light_source(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read the x light value */
	geo->light.x = u2f(*input++);

	/* read the y light value */
	geo->light.y = u2f(*input++);

	/* read the z light value */
	geo->light.pz = u2f(*input++);

	return input;
}

/* Command 0B: Transformation Matrix Write */
GeoPtr Geo::geo_matrix_write(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  i;

	(void)opcode;

	/* read in the transformation matrix */
	for (i = 0; i < 12; i++)
		geo->matrix[i] = u2f(*input++);

	return input;
}

/* Command 0C: Parallel Transfer Vector Write */
GeoPtr Geo::geo_translate_write(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  i;

	(void)opcode;

	/* read in the translation vector */
	for (i = 0; i < 3; i++)
		geo->matrix[i+9] = u2f(*input++);

	return input;
}

/* Command 0D: Geo Data Memory Push (undocumented, unsupported) */
GeoPtr Geo::geo_data_mem_push(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  address, count, i;

	/*
	    This command pushes data stored in the Geometry DSP's RAM
	    to the hardware 3D rasterizer. Since we don't emulate the
	    DSP, we don't know what the RAM contents are.

	    Eventually, we could check for the address, and if it
	    happens to point to a polygon ROM, we could potentially
	    emulate it partially.

	    No games are known to use this command yet.
	*/


	(void)opcode;

	/* read in the address */
	address = *input++;

	/* read in the count */
	count = *input++;


	(void)i;
/*
    for (i = 0; i < count; i++)
        model2_3d_push(0);
*/

	return input;
}

/* Command 0E: Geo Test */
GeoPtr Geo::geo_test(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t      data, blocks, address, count, checksum, i;

	(void)opcode;

	/* fifo test */
	data = 1;

	for (i = 0; i < 32; i++)
	{
		if (*input++ != data)
		{
			/* TODO: Set Red LED on */
				}

		data <<= 1;
	}

	/* get the number of checksums we have to run */
	blocks = *input++;

#ifdef M2_DC_MEMORY
	// Each block is three words (address, count, checksum) and its sums have
	// no effect (the LEDs are not emulated). The desktop's compiler reduces
	// this whole loop to the cursor moving on 3 * blocks words; here every
	// read goes through the RomSource check and cannot be dropped, and blocks
	// can be huge (frame 189 of the attract mode: an endless-looking loop on
	// the SH-4). The same result, without the loop.
	(void)address;
	(void)count;
	(void)checksum;
	input += 3 * blocks;
	return input;
#endif

	for (i = 0; i < blocks; i++)
	{
		uint32_t  sum_even, sum_odd, j;

		/* read in the address */
		address = (*input++) & 0x7fffff;

		/* read in the count */
		count = *input++;

		/* read in the checksum */
		checksum = *input++;

		/* reset the checksum counters */
		sum_even = 0;
		sum_odd = 0;

		for (j = 0; j < count; j++)
		{
#ifdef M2_DC_MEMORY
			data = geo->rom->dword(RomRegion::Polygons, address++ * 4);
#else
			data = geo->polygon_rom[address++];
#endif

			address &= geo->polygon_rom_mask;

			sum_even += data >> 16;
			sum_even &= 0xffff;

			sum_odd += data & 0xffff;
			sum_odd &= 0xffff;
		}

		sum_even += checksum >> 16;
		sum_even &= 0xffff;

		sum_odd += checksum & 0xffff;
		sum_odd &= 0xffff;

		if (sum_even != 0 || sum_odd != 0)
		{
			/* TODO: Set Green LED on */
				}
	}

	return input;
}

/* Command 0F: End */
GeoPtr Geo::geo_end(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	(void)opcode;

	/* signal the end of this data block the rasterizer */
	model2_3d_push(raster, 0xff000000);

	/* signal end by returning nullptr */
	return GeoPtr{};
}

/* Command 10: Dummy */
GeoPtr Geo::geo_dummy(geo_state *geo, uint32_t opcode, GeoPtr input)
{
//  uint32_t  data;
	(void)opcode;

	/* do the dummy read cycle */
//  data = *input++;
	input++;

	return input;
}

/* Command 14: Log Data Write */
GeoPtr Geo::geo_log_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;
	uint32_t  i, count;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the starting address/dsp id */
	model2_3d_push(raster, *input++);

	/* get the count */
	count = *input++;

	/* push the count */
	model2_3d_push(raster, count << 2);

	/* loop and send the data */
	for (i = 0; i < count; i++)
	{
		uint32_t  data = *input++;

		model2_3d_push(raster, data & 0xff);
		model2_3d_push(raster, (data >> 8) & 0xff);
		model2_3d_push(raster, (data >> 16) & 0xff);
		model2_3d_push(raster, (data >> 24) & 0xff);
	}

	return input;
}

/* Command 16: LOD */
GeoPtr Geo::geo_lod(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read in the LOD */
	geo->lod = u2f(*input++);

	return input;
}

/* Command 1D: Code Upload  (undocumented, unsupported) */
GeoPtr Geo::geo_code_upload(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  count, i;

	/*
	    This command uploads code to program memory and
	    optionally runs it. Probably used for debugging.

	    No games are known to use this command yet.
	*/


	(void)opcode;

	/* read in the flags */
//  flags = *input++;
	input++;

	/* read in the count */
	count = *input++;

	for (i = 0; i < count; i++)
	{
		[[maybe_unused]] u64  code;

		/* read the top part of the opcode */
		code = *input++;

		code <<= 32;

		/* the bottom part comes in two pieces */
		code |= *input++;
		code |= (*input++) << 16;
	}

	/*
	    Bit 10 of flags indicate whether to run iummediately after upload
	*/

/*
    if (flags & 0x400)
        code_jump();
*/

	return input;
}

/* Command 1E: Code Jump (undocumented, unsupported) */
GeoPtr Geo::geo_code_jump(geo_state *geo, uint32_t opcode, GeoPtr input)
{
//  uint32_t  address;

	/*
	    This command jumps to a specified address in program
	    memory. Code can be uploaded with function 1D.
	    Probably used for debugging.

	    No games are known to use this command yet.
	*/


	(void)opcode;

//  address = *input++ & 0x3ff;
	input++;

/*
    code_jump(address)
*/
	return input;
}

GeoPtr Geo::geo_process_command(geo_state *geo, uint32_t opcode, GeoPtr input, bool *end_code)
{
	switch ((opcode >> 23) & 0x1f)
	{
		case 0x00: input = geo_nop(geo, opcode, input);                   break;
		case 0x01: input = geo_object_data(geo, opcode, input);           break;
		case 0x02: input = geo_direct_data(geo, opcode, input);           break;
		case 0x03: input = geo_window_data(geo, opcode, input);           break;
		case 0x04: input = geo_texture_data(geo, opcode, input);          break;
		case 0x05: input = geo_polygon_data(geo, opcode, input);          break;
		case 0x06: input = geo_texture_parameters(geo, opcode, input);    break;
		case 0x07: input = geo_mode(geo, opcode, input);                  break;
		case 0x08: input = geo_zsort_mode(geo, opcode, input);            break;
		case 0x09: input = geo_focal_distance(geo, opcode, input);        break;
		case 0x0a: input = geo_light_source(geo, opcode, input);          break;
		case 0x0b: input = geo_matrix_write(geo, opcode, input);          break;
		case 0x0c: input = geo_translate_write(geo, opcode, input);       break;
		case 0x0d: input = geo_data_mem_push(geo, opcode, input);         break;
		case 0x0e: input = geo_test(geo, opcode, input);                  break;
		case 0x0f: input = geo_end(geo, opcode, input); *end_code = true; break;
		case 0x10: input = geo_dummy(geo, opcode, input);                 break;
		case 0x11: input = geo_object_data(geo, opcode, input);           break;
		case 0x12: input = geo_direct_data(geo, opcode, input);           break;
		case 0x13: input = geo_window_data(geo, opcode, input);           break;
		case 0x14: input = geo_log_data(geo, opcode, input);              break;
		case 0x15: input = geo_polygon_data(geo, opcode, input);          break;
		case 0x16: input = geo_lod(geo, opcode, input);                   break;
		case 0x17: input = geo_mode(geo, opcode, input);                  break;
		case 0x18: input = geo_zsort_mode(geo, opcode, input);            break;
		case 0x19: input = geo_focal_distance(geo, opcode, input);        break;
		case 0x1a: input = geo_light_source(geo, opcode, input);          break;
		case 0x1b: input = geo_matrix_write(geo, opcode, input);          break;
		case 0x1c: input = geo_translate_write(geo, opcode, input);       break;
		case 0x1d: input = geo_code_upload(geo, opcode, input);           break;
		case 0x1e: input = geo_code_jump(geo, opcode, input);             break;
		case 0x1f: input = geo_end(geo, opcode, input); *end_code = true; break;
	}

	return input;
}

void Geo::parse(uint32_t read_start)
{
    const char *writer_trace = write_provenance_ ? std::getenv("M2_WRITE_TRACE") : nullptr;
    const auto *traced_provenance = writer_trace && *writer_trace ? write_provenance_ : nullptr;
	if (camera_lineage_) camera_lineage_->begin_scene();
	uint32_t  address = (read_start & 0x1ffff)/4;
	GeoPtr input = buf(address);
	uint32_t  opcode;
	uint32_t  op_count = 0;
	bool end_code = false;

	// reset raster frame variables
	render_frame_start();
	if (pose_presentation_ || track_shaft_route_) rotate_pose_frame();
	else if (instance_reference_) reference_primitives_.clear();

	while (end_code == false && input.i < 0x20000/4 && op_count++ < 0x8000)
	{
		/* Record before consuming the opcode, including after a buffer jump. */
        const uint32_t command_word = input.i;
		command_word_ = command_word;
		opcode = *input++;

		/* if it's a jump opcode, do the jump */
		if (opcode & 0x80000000)
		{
			/* get the address */
			address = (opcode & 0x1ffff) / 4;

			/* update our pointer */
			input = buf(address);

			/* go again */
			continue;
		}

		/* process it */
        m2trace::command_writers(command_word, opcode, traced_provenance);
        m2trace::command_begin(command_word, opcode);
		input = geo_process_command(&geo_, opcode, input, &end_code);
	}
	if (instance_reference_) replay_instance_reference();
}

/***********************************************************************************************/

} // namespace rt

