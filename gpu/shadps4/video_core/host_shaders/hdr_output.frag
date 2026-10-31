// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: HDR10 output for a game that makes an SDR picture (HostPasses::HdrPass). The finished
// frame (sRGB encoded) is shown at paper white. Where it equals the game's picture before its UI
// (the 3D scene), the highlights smoothly expand towards the display's peak brightness (peak_nits)
// using a continuous, noise-free highlight shoulder curve.
// Diffuse sky, road, and vehicle surfaces below the highlight knee remain 1:1 bit-exact to SDR
// presentation, while direct sunlight and specular highlights smoothly expand with C2 continuity.
// The UI stays at paper white. Out: Rec.2020 primaries, PQ (SMPTE ST 2084) encoded.
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D frame_tex;    // the frame, sRGB encoded values (UNORM)
layout(binding = 1) uniform sampler2D hudless_tex;  // the picture before the UI, the same way
layout(binding = 2) uniform sampler2D scene_tex;    // unused
layout(binding = 3, r32f) uniform readonly image2D state;  // unused

layout(push_constant) uniform Params {
    float paper_nits;   // SDR white (UI, menus)
    float peak_nits;    // the brightest highlight
    float has_hudless;  // 0: no 3D scene in this frame (menus): everything at paper white
    float knee;         // SDR luminance where the highlights start
    float has_scene;    // unused
};

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

vec3 ToLinear(vec3 c) {
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThan(c, vec3(0.04045)));
}

vec3 Pq(vec3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const vec3 y = pow(clamp(nits / 10000.0, 1e-6, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

// Rec.709 to Rec.2020 primaries (columns).
const mat3 kBt709ToBt2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                                 0.3292820, 0.9195400, 0.0880132,
                                 0.0433136, 0.0113612, 0.8955950);

void main() {
    const vec3 sdr = clamp(texture(frame_tex, uv).rgb, vec3(0.0), vec3(1.0));
    const vec3 lin = ToLinear(sdr);

    // The UI: only high-contrast HUD overlays (speedometer, lap timer, map text).
    // Subtle differences below 0.12 come from bloom, post-processing or rounding
    // differences between the display and pre-UI passes and must never trigger UI
    // detection, avoiding luminance noise and tearing around the sunlight.
    float ui = 0.0;
    if (has_hudless > 0.5) {
        const vec3 d = abs(sdr - clamp(texture(hudless_tex, uv).rgb, vec3(0.0), vec3(1.0)));
        ui = smoothstep(0.12, 0.22, max(d.r, max(d.g, d.b)));
    } else {
        ui = 1.0; // Menus and loading screens stay at paper white
    }

    // In units of SDR white: smooth highlight expansion towards peak_nits / paper_nits.
    // Content below the highlight knee (diffuse sky, landscape, car bodies) stays 1:1
    // bit-exact to native SDR at paper white. Only direct sunlight, the sun disc corona,
    // and bright specular highlights expand towards peak brightness using a C2-continuous
    // smootherstep shoulder curve, ensuring perfectly clean, noise-free presentation.
    const float range = max(peak_nits / max(paper_nits, 1e-3), 1.0);
    const float y = clamp(dot(lin, kLuma), 0.0, 1.0);
    const float t = clamp((y - knee) / max(1.0 - knee, 1e-3), 0.0, 1.0);
    const float shoulder = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    const float highlight = (range - 1.0) * shoulder;
    const vec3 hdr = lin * (1.0 + highlight);

    const vec3 nits = paper_nits * mix(hdr, lin, ui);
    color = vec4(Pq(kBt709ToBt2020 * max(nits, vec3(0.0))), 1.0);
}
