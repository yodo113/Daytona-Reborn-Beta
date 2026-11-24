// Ray-traced sun shadows, step 2 of dxr.hlsl: one ray per pixel towards the
// sun with an inline ray query (DXR 1.1, shader model 6.5). Own root
// signature: the D3D12 pipeline is made by app/gpu/dxr_scene.cpp, not SDL_GPU.
//
// Space: the rasterizer's camera space (x and y times the focal length, so
// screen = origin + x / z). A pixel's point is rebuilt from its view z and
// its screen position; `sun` is the light vector already scaled the same way.

#define ROOT "RootConstants(num32BitConstants=23, b0), SRV(t0), DescriptorTable(SRV(t1), UAV(u0)), SRV(t2), SRV(t3), UAV(u1)"

// Cut-out triangles (renderer 3: fir trees, fences) are the structure's
// second, non-opaque geometry: a ray passes where their texel is the
// transparent one (15), as m2.hlsl ps_poly discards it. Level 0, nearest.
StructuredBuffer<uint> texram : register(t2); // both sheets, as m2.hlsl
struct Cutout {
    uint info_x, info_y; // m2.hlsl PolyIn info.x, info.y
    float2 uv[3];        // u, v at each corner (ps_poly's u, v: 256 per texel)
};
StructuredBuffer<Cutout> cutouts : register(t3);

// m2.hlsl get_texel
uint get_texel(uint base_x, uint base_y, int x, int y, uint sheet) {
    int x2 = int(base_x) + x;
    int y2 = int(base_y) + y;
    if (x2 >= 1024) {
        x2 -= 1024;
        y2 ^= 1024;
    }
    const uint offset = uint((y2 / 2) * 512 + (x2 / 2));
    uint texel = texram[sheet * 0x80000u + ((offset >> 1) & 0x7ffffu)];
    if (offset & 1u) texel >>= 16;
    if ((y & 1) == 0) texel >>= 8;
    if ((x & 1) == 0) texel >>= 4;
    return texel & 0x0fu;
}

bool cutout_solid(Cutout c, float2 bary) {
    const float2 uv = c.uv[0] * (1.0 - bary.x - bary.y) + c.uv[1] * bary.x + c.uv[2] * bary.y;
    int u = int(uv.x), v = int(uv.y);
    const uint flags = c.info_x;
    const uint w = 32u << ((flags >> 17) & 7u), h = 32u << ((flags >> 20) & 7u);
    if (((flags >> 5) & 1u) && (u & int(w << 8)) != 0) u = ~u;
    if (((flags >> 6) & 1u) && (v & int(h << 8)) != 0) v = ~v;
    const int x = int(uint(u >> 8) & (w - 1u)), y = int(uint(v >> 8) & (h - 1u));
    const uint tex_x = ((c.info_y & 0xffffu) - 2048u) & 2047u, tex_y = ((c.info_y >> 16) - 1024u) & 1023u;
    return get_texel(tex_x, tex_y, x, y, ((flags >> 10) & 1u) ? 1u : 0u) != 0x0fu;
}

struct Params {
    uint2 size;        // the target, in pixels
    float2 to_screen;  // screen units per pixel (x, y)
    float2 origin;     // screen position of the camera axis
    float strength;    // light left in shadow (0 black .. 1 none)
    float bias;        // ray start offset, a fraction of the view z
    float3 sun;        // towards the sun, in the scaled camera space
    float sky;         // sky light: the share of the light it gives (0 = off)
    float2 focus;      // the focal lengths: scaled space = (x * focus.x, y * focus.y, z)
    float sky_radius;  // how far a sky ray looks for an occluder, camera units
    float sun_radius;  // the sun's disc, angular radius in radians (soft edges; 0 = a point)
    float shaft;       // light shafts: their strength (0 = off)
    float shaft_dist;  // how far an eye ray is marched for them, camera units (z)
    // The player's car's own camera (the game draws it with another centre
    // and focal length): its pixels carry a negative view z (ps_view_z2).
    float2 origin2;
    float2 focus2;
    float shaft_phase; // fraction of a sample step travelled by the camera
};
ConstantBuffer<Params> p : register(b0);
RaytracingAccelerationStructure scene : register(t0);
Texture2D<float> view_z : register(t1);
RWTexture2D<unorm float2> light : register(u0); // light, light shafts

// True when the segment from `from` along `along` (t in 0..1) meets a solid
// surface (scaled camera space, cut-out holes let it through).
bool blocked(float3 from, float3 along, float tmin, float tmax) {
    RayDesc ray;
    ray.Origin = from;
    ray.Direction = along;
    ray.TMin = tmin;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
    while (q.Proceed()) // only cut-out triangles come back as candidates
        if (cutout_solid(cutouts[q.CandidatePrimitiveIndex()], q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// The pixel's point in the true camera space (focal length divided out).
// A negative z is the player's car, seen through its own camera.
float3 true_point(int2 xy, float z) {
    const float2 screen = (float2(xy) + 0.5) * p.to_screen;
    const bool car = z < 0.0;
    const float2 o = car ? p.origin2 : p.origin, f = car ? p.focus2 : p.focus;
    z = abs(z);
    return float3((screen.x - o.x) * z / f.x, (o.y - screen.y) * z / f.y, z);
}

static const uint kSkyRays = 6;
groupshared float sky_block[8][8]; // one sky value per 2x2 block of the 16x16 group
groupshared float z_block[8][8];   // its view z

// Sky light (ambient occlusion) at a point: the surface's normal from the
// neighbours' points (the nearer one on each axis, so an edge does not bend
// it), then rays over the hemisphere, cosine-weighted and rotated per block,
// leaving from just above the surface; a ray blocked within sky_radius sees
// no sky. 1 = all the sky.
float sky_at(int2 c, float z, float3 at_true, uint grid_shift, uint rays) {
    // signed: a neighbour on the player's car is rebuilt with its camera
    const float zl = view_z[c - int2(1, 0)], zr = view_z[c + int2(1, 0)];
    const float zu = view_z[c - int2(0, 1)], zd = view_z[c + int2(0, 1)];
    const float3 dx = abs(abs(zr) - z) < abs(abs(zl) - z) && zr != 0.0
        ? true_point(c + int2(1, 0), zr) - at_true
        : at_true - true_point(c - int2(1, 0), abs(zl) > 1e-6 ? zl : 1e-6);
    const float3 dy = abs(abs(zd) - z) < abs(abs(zu) - z) && zd != 0.0
        ? true_point(c + int2(0, 1), zd) - at_true
        : at_true - true_point(c - int2(0, 1), abs(zu) > 1e-6 ? zu : 1e-6);
    float3 n = normalize(cross(dx, dy));
    if (!all(isfinite(n))) return 1.0;
    if (dot(n, at_true) > 0.0) n = -n; // towards the eye
    const float3 scale = float3(p.focus, 1.0);
    // the rebuilt point can sit just under its surface: leave from above it
    const float3 from = (at_true + n * max(0.004 * z, 0.02)) * scale;
    const float3 t = normalize(abs(n.y) < 0.9 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    const float3 b = cross(n, t);
    const uint hash = (uint(c.x >> grid_shift) * 73856093u) ^ (uint(c.y >> grid_shift) * 19349663u);
    const float spin = float(hash & 1023u) / 1024.0 * 6.2831853;
    uint open = 0;
    [unroll] for (uint k = 0; k < rays; ++k) {
        const float r = sqrt((float(k) + 0.5) / float(rays));
        const float a = spin + float(k) * 2.3999632; // golden angle
        const float3 d = t * (r * cos(a)) + b * (r * sin(a)) + n * sqrt(1.0 - r * r);
        if (!blocked(from, d * p.sky_radius * scale, 0.0, 1.0)) ++open;
    }
    return float(open) / float(rays);
}

// The sun at a point: 1 lit, p.strength in shadow. The sun is a disc, not a
// point: two rays towards points of it (rotated with `noise`) give the soft
// edge an occluder's shadow has, wider the farther the occluder.
float sun_at(float3 at_true, float z, float noise) {
    const float3 scale = float3(p.focus, 1.0);
    const float3 at = at_true * scale;
    if (p.sun_radius <= 0.0) return blocked(at, p.sun, p.bias * z, 1e7) ? p.strength : 1.0;
    const float3 s0 = normalize(p.sun / scale); // the true direction
    const float3 u = normalize(abs(s0.y) < 0.9 ? cross(s0, float3(0, 1, 0)) : cross(s0, float3(1, 0, 0)));
    const float3 v = cross(s0, u);
    const float tan_r = tan(p.sun_radius);
    float lit = 0.0;
    [unroll] for (uint k = 0; k < 2; ++k) {
        const float a = (noise + 0.5 * float(k)) * 6.2831853;
        const float r = tan_r * sqrt(frac(noise * 1.618034 + 0.5 * float(k)));
        const float3 d = normalize(s0 + (u * cos(a) + v * sin(a)) * r) * scale;
        lit += blocked(at, d, p.bias * z / max(length(d), 1e-6) * length(p.sun), 1e7) ? p.strength : 1.0;
    }
    return lit * 0.5;
}

// interleaved gradient noise: an even spread over neighbouring cells
float ign(int2 c) { return frac(52.9829189 * frac(0.06711056 * float(c.x) + 0.00583715 * float(c.y))); }

groupshared float sun_block[8][8]; // the sun once per 2x2 block, as the sky

// Light shafts (23/11: the sun through the Canyon tunnel's openings,
// seen in the air). The eye ray through pixel `c` is marched up to the
// surface (or shaft_dist), each sample asking if the sun reaches it. Sunlit
// air between shadows is what shows: the haze is lit share times shadowed
// share (4 f (1 - f), at most 1), so air all in the sun (open road) or all in
// the shade adds nothing. Scaled by how deep the ray went (full from 0.4
// shaft_dist).
static const uint kShaftSteps = 12;
RWStructuredBuffer<float> haze_lo : register(u1); // the shafts, one per light-shaft sample cell
float shafts_at(int2 c, float z) {
    const float far_z = z > 0.0 ? min(z, p.shaft_dist) : p.shaft_dist;
    const float3 dir = true_point(c, 1.0); // the eye ray, z = 1
    const float3 scale = float3(p.focus, 1.0);
    const float step_z = p.shaft_dist / float(kShaftSteps);
    const float phase = frac(p.shaft_phase);
    float lit_length = 0.0, last_t = 0.0;
    bool last_lit = false;
    [loop] for (uint k = 0; k < kShaftSteps; ++k) {
        const float t = (float(k + 1) - phase) * step_z;
        if (t > far_z) break;
        last_lit = !blocked(dir * t * scale, p.sun, 0.0, 1e7);
        if (last_lit) lit_length += t - last_t;
        last_t = t;
    }
    if (last_t == 0.0) return 0.0;
    if (last_lit) lit_length += far_z - last_t;
    const float f = saturate(lit_length / far_z);
    // Air scatters sunlight mostly forward: shafts show looking towards the
    // sun and hardly with the sun behind (a forest seen with the sun at the
    // back was a white fog, 23/11). ((1 + cos) / 2)^1.5: 1 facing the sun,
    // 0.35 across it, 0.1 at 115 degrees, 0 straight away from it.
    const float facing = dot(normalize(dir), normalize(p.sun / scale));
    const float forward = pow(saturate(0.5 + 0.5 * facing), 1.5);
    return p.shaft * forward * min(4.0 * f * (1.0 - f) * far_z / (0.4 * p.shaft_dist), 1.0);
}

void shade_pixel(uint3 id, uint3 local, uint pixel_scale, uint shaft_stride, uint sky_rays) {
    const uint2 base = id.xy * pixel_scale;
    const int2 c = int2(min(base + pixel_scale - 1u, p.size - 1u));
    const bool inside = all(base < p.size);
    const float z_signed = inside ? view_z[c] : 0.0; // < 0: the player's car
    const float z = abs(z_signed);
    const float3 at_true = true_point(c, z_signed);
    // One work item per output pixel at large 2x targets. Neighbouring
    // work items still share the sky and sun samples in a 2x2 block.
    if (all((local.xy & 1u) == 0u)) {
        const int2 b = int2(local.xy >> 1);
        z_block[b.y][b.x] = pixel_scale == 1 ? z : z_signed;
        sun_block[b.y][b.x] = z > 0.0 ? sun_at(at_true, z, ign(int2(id.xy >> 1))) : 1.0;
        if (p.sky > 0.0) sky_block[b.y][b.x] = z > 0.0 ? sky_at(c, z, at_true, pixel_scale, sky_rays) : 1.0;
    }
    GroupMemoryBarrierWithGroupSync();
    // Reconstruct the light shafts from the grid with bilinear filtering.
    float haze = 0.0;
    if (p.shaft > 0.0) {
        const uint2 lo = (p.size + shaft_stride - 1u) / shaft_stride;
        const float2 g = clamp((float2(base) + 0.5 * float(pixel_scale)) / float(shaft_stride) - 0.5, 0.0, float2(lo - 1u));
        const uint2 g0 = uint2(g), g1 = min(g0 + 1u, lo - 1u);
        const float2 w = g - float2(g0);
        const float a = lerp(haze_lo[g0.y * lo.x + g0.x], haze_lo[g0.y * lo.x + g1.x], w.x);
        const float b = lerp(haze_lo[g1.y * lo.x + g0.x], haze_lo[g1.y * lo.x + g1.x], w.x);
        haze = lerp(a, b, w.y);
    }
    if (!inside) return;

    // Average nearby samples on this surface only. Reject samples across
    // silhouettes, including the player's car, which has its own camera.
    const int2 b = int2(local.xy >> 1);
    const float depth = pixel_scale == 1 ? z : z_signed;
    float sun_sum = 0.0, sky_sum = 0.0, weight = 0.0;
    if (z > 0.0) {
        [unroll] for (int j = -1; j <= 1; ++j)
            [unroll] for (int i = -1; i <= 1; ++i) {
                const int2 o = clamp(b + int2(i, j), 0, 7);
                const float w = abs(z_block[o.y][o.x] - depth) <= 0.05 * z ? 1.0 : 0.0;
                sun_sum += w * sun_block[o.y][o.x];
                if (p.sky > 0.0) sky_sum += w * sky_block[o.y][o.x];
                weight += w;
            }
    }
    const float sun = weight > 0.0 ? sun_sum / weight : z > 0.0 ? sun_at(at_true, z, ign(c)) : 1.0;
    const float sky = p.sky <= 0.0 ? 1.0 : weight > 0.0 ? sky_sum / weight : sky_block[b.y][b.x];
    const float lit = saturate((sun - p.strength) / max(1.0 - p.strength, 1e-3));
    const float2 packed = z > 0.0 ? float2(sun * (1.0 - p.sky + p.sky * sky), haze * (1.0 - lit))
                                  : float2(1.0, 0.0);
    if (pixel_scale == 1) {
        light[c] = packed;
        return;
    }

    // Keep the full-resolution depth mask. In a pixel split across two
    // surfaces, trace the sun from the other surface rather than spreading
    // light across a railing, HUD edge or the silhouette of the player's car.
    [unroll] for (uint y = 0; y < 2; ++y)
        [unroll] for (uint x = 0; x < 2; ++x) {
            const int2 q = int2(base + uint2(x, y));
            if (any(uint2(q) >= p.size)) continue;
            const float other = view_z[q];
            if (other == 0.0) {
                light[q] = float2(1.0, 0.0);
            } else if (abs(other - z_signed) <= 0.05 * z) {
                light[q] = packed;
            } else {
                const float other_z = abs(other);
                const float3 at_other = true_point(q, other);
                const float sun_other = sun_at(at_other, other_z, ign(q));
                const float sky_other = p.sky > 0.0 ? sky_at(q, other_z, at_other, 2, sky_rays) : 1.0;
                const float lit_other = saturate((sun_other - p.strength) / max(1.0 - p.strength, 1e-3));
                light[q] = float2(sun_other * (1.0 - p.sky + p.sky * sky_other), haze * (1.0 - lit_other));
            }
        }
}

[RootSignature(ROOT)]
[numthreads(16, 16, 1)]
void cs_shadow(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID) {
    shade_pixel(id, local, 1, 8, kSkyRays);
}

[RootSignature(ROOT)]
[numthreads(16, 16, 1)]
void cs_shadow_large(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID) {
    shade_pixel(id, local, 2, 12, kSkyRays);
}

void write_shafts(uint3 id, uint stride) {
    const uint2 lo = (p.size + stride - 1u) / stride;
    if (any(id.xy >= lo)) return;
    const int2 c = min(int2(id.xy * stride + stride / 2u), int2(p.size) - 1);
    haze_lo[id.y * lo.x + id.x] = shafts_at(c, abs(view_z[c]));
}

[RootSignature(ROOT)]
[numthreads(8, 8, 1)]
void cs_shafts(uint3 id : SV_DispatchThreadID) {
    write_shafts(id, 8);
}

[RootSignature(ROOT)]
[numthreads(8, 8, 1)]
void cs_shafts_large(uint3 id : SV_DispatchThreadID) {
    write_shafts(id, 12);
}
