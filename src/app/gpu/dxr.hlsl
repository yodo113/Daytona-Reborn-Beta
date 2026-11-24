// Ray-traced sun shadows, Direct3D
// 12 only. scripts/build_dxr_shaders.py compiles it with the Windows SDK's DXC
// to DXIL and writes src/app/gpu/dxr_shaders_gen.h.
//
// Three steps, only when the option is on (off, none of this runs and the
// image is the one m2.hlsl draws):
//  1. ps_view_z, after the frame's pass: the same polygons again with the
//     depth test EQUAL against that pass's depth (each polygon has its own
//     depth, its place in the draw order), so exactly the polygon that won a
//     pixel writes its view z there; ps_front_z writes 0 (no shadow) where a
//     front tilemap layer covers the 3D.
//  2. cs_shadow (own D3D12 pipeline, inline ray query): from each pixel's
//     camera-space point, one ray towards the sun through the frame's
//     acceleration structure; a hit leaves the pixel's light at `strength`.
//  3. ps_shade: the frame multiplied by that light, plus the light shafts.
#include "m2.hlsl"

// 1. view z of the polygon that won the pixel
float ps_view_z(PolyOut i) : SV_Target { return 1.0 / i.oz.x; }
// The player's car, drawn by the game through its own camera (another centre
// and focal length): its view z negated, so cs_shadow rebuilds its points
// with that camera (Params origin2, focus2).
float ps_view_z2(PolyOut i) : SV_Target { return -1.0 / i.oz.x; }

float ps_front_z(QuadOut i) : SV_Target {
    const int2 xy = tile_position(i.pos.xy, int(tiledata[0]));
    const int x = xy.x, y = xy.y;
    if (x < 0 || x >= kTileW) discard;
    uint pen;
    for (uint l = 1u; l < 8u; l += 2u)
        if (tile_layer(l, false, x, y, pen)) return 0.0;
    discard;
    return 0.0;
}

float ps_quad_z(QuadOut i) : SV_Target {
    const float4 c = layer_tex.Sample(layer_smp, i.uv);
    if (all(c == 0.0)) discard;
    return 0.0;
}

// 3. the frame times the light (alpha, blended as dst * src.a), plus the
// light shafts in the sun's warm white (rgb, added)
float4 ps_shade(QuadOut i) : SV_Target {
    const float2 l = layer_tex.Sample(layer_smp, i.uv).rg;
    return float4(l.g * float3(1.0, 0.95, 0.82), l.r);
}
