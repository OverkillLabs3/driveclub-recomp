#version 450
// A cinematic grade on the linear HDR image, before the game's own bloom and tone mapping.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;   // linear HDR (RGBA16F)
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float pad;
    vec4 values[4];
} p;

float hash(vec2 x) { return fract(sin(dot(x, vec2(12.9898, 78.233))) * 43758.5453); }

void main() {
    vec3 c = texture(picture, uv).rgb;
    float exposure   = p.values[0].x;   // stops, 0 = unchanged
    float contrast   = p.values[0].y;   // 1 = unchanged
    float saturation = p.values[0].z;   // 1 = unchanged
    float vignette   = p.values[0].w;   // 0 = none
    float grain      = p.values[1].x;   // 0 = none
    float split      = p.values[1].y;   // strength of teal shadows / orange highlights

    c *= exp2(exposure);
    // contrast around middle grey, in linear light
    c = 0.18 * pow(max(c / 0.18, vec3(1e-4)), vec3(contrast));
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, saturation);
    // split toning: shadows toward teal, highlights toward orange
    float w = smoothstep(0.02, 1.5, luma);
    vec3 teal = vec3(0.80, 1.00, 1.10), orange = vec3(1.12, 1.00, 0.82);
    c *= mix(vec3(1.0), mix(teal, orange, w), split);
    // vignette
    vec2 d = uv - 0.5;
    d.x *= p.resolution.x / p.resolution.y;
    c *= 1.0 - vignette * smoothstep(0.35, 1.1, length(d));
    // film grain, stronger in the shadows
    float n = hash(uv * p.resolution + fract(p.time) * 61.7) - 0.5;
    c *= 1.0 + n * grain * (1.0 - 0.6 * w);
    color = vec4(max(c, vec3(0.0)), 1.0);
}
