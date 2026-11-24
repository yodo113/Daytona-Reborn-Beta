// Fullscreen copy of an 8-bit sRGB texture into PQ. Same chain as
// app::hdr10_pq_from_srgb8 in src/app/hdr10_encode.h. The texture is UNORM:
// Sample returns the stored code divided by 255, and this shader decodes sRGB.
// Register layout matches the lens pass in home.hlsl.

struct Quad {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Quad vs_hdr10(uint vertex : SV_VertexID) {
    Quad o;
    o.uv = float2((vertex << 1) & 2, vertex & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

#ifdef __spirv__
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]]
#endif
Texture2D<float4> scene : register(t0, space2);
#ifdef __spirv__
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]]
#endif
SamplerState source : register(s0, space2);

cbuffer Hdr10Settings : register(b0, space3) {
    float paper_white_nits;
    // Display curve for the game's levels. 0: the sRGB curve of a PC
    // display (the reference). Otherwise a power law, as on an arcade CRT
    // (about 2.4): light = level^gamma.
    float display_gamma;
    // Colour saturation in light (1 = unchanged): each colour moves away
    // from the grey of the same luminance (BT.709 weights), not clipped here.
    float display_saturation;
};

float3 saturate_light(float3 light) {
    const float y = dot(light, float3(0.2126, 0.7152, 0.0722));
    return max(y + (light - y) * display_saturation, 0.0);
}

// app::srgb_to_linear. Below 0.04045 the slope is 1/12.92.
float srgb_to_linear(float encoded) {
    if (encoded <= 0.04045) return encoded / 12.92;
    return pow((encoded + 0.055) / 1.055, 2.4);
}

// app::linear_to_srgb.
float linear_to_srgb(float light) {
    if (light <= 0.0031308) return light * 12.92;
    return 1.055 * pow(light, 1.0 / 2.4) - 0.055;
}

// The light a level stands for, through the chosen display curve.
float level_to_linear(float level) {
    return display_gamma > 0.0 ? pow(max(level, 0.0), display_gamma) : srgb_to_linear(level);
}

// app::bt709_to_bt2020, rows of kBt709ToBt2020.
float3 bt709_to_bt2020(float3 color) {
    const float3x3 kBt709ToBt2020 = float3x3(
        0.6274, 0.3293, 0.0433,
        0.0691, 0.9195, 0.0114,
        0.0164, 0.0880, 0.8956);
    return mul(kBt709ToBt2020, color);
}

// app::hdr10_pq. The application supplies the paper white in cd/m2.
float hdr10_pq(float linear_sdr) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float nits = clamp(linear_sdr, 0.0, 1.0) * paper_white_nits;
    const float shaped = pow(nits / 10000.0, m1);
    return pow((c1 + c2 * shaped) / (1.0 + c3 * shaped), m2);
}

float4 ps_hdr10(Quad i) : SV_Target {
    const float4 texel = scene.Sample(source, i.uv);
    const float3 container = bt709_to_bt2020(saturate_light(
        float3(level_to_linear(texel.r), level_to_linear(texel.g), level_to_linear(texel.b))));
    return float4(hdr10_pq(container.r), hdr10_pq(container.g), hdr10_pq(container.b), 1);
}

// SDR output with the display curve: the levels are re-encoded for the
// sRGB display, so that it shows the light the chosen curve gives them.
float4 ps_sdr_curve(Quad i) : SV_Target {
    const float4 texel = scene.Sample(source, i.uv);
    const float3 light = min(saturate_light(
        float3(level_to_linear(texel.r), level_to_linear(texel.g), level_to_linear(texel.b))), 1.0);
    return float4(linear_to_srgb(light.r), linear_to_srgb(light.g), linear_to_srgb(light.b), 1);
}
