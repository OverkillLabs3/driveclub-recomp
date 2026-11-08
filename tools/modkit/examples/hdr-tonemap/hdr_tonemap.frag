#version 450
// Tone mapping curves for the linear HDR image: exposure in stops, then one of several curves that bring the scene into
// 0..1 before the game's own grading (its colour lookup table). Curve 0 leaves the picture alone, 1 Reinhard with a white
// point, 2 ACES (Narkowicz fit), 3 Hable (Uncharted 2), 4 a soft shoulder that keeps the game's mid tones, 5 shows in
// magenta where the scene is brighter than 1 (what the game would clip).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;
layout(binding = 1) uniform sampler2D linear_depth;
layout(binding = 2) uniform sampler2D motion;
layout(binding = 3) uniform sampler2D gbuffer;
layout(binding = 4) uniform sampler2D params;
layout(binding = 5) uniform sampler2D scene;
layout(binding = 6) uniform sampler2D history;       // this slot's picture of the frame before; alpha holds the exposure
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;
    vec4 values[4];
    float has_history;
} p;

vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 hable_(vec3 x) { const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30; return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F; }
float luma(vec3 c) { return dot(c, vec3(0.0722, 0.7152, 0.2126)); }

void main() {
    vec3 c = max(texture(picture, uv).rgb, vec3(0.0));
    if (p.has_depth < 0.5) { color = vec4(c, 1.0); return; }   // the finished picture is left alone
    float curve = p.values[0].x, exposure = p.values[0].y, white = max(p.values[0].z, 1.0), sat = p.values[0].w;
    float shoulder = p.values[1].x;
    float auto_strength = p.values[1].y;   // 0 off .. 1: exposure follows the scene's average brightness
    float auto_target = p.values[1].z;     // the average brightness the picture is brought to
    float state_alpha = 1.0;
    if (auto_strength > 0.0) {
        float sum = 0.0;
        for (int j = 0; j < 5; ++j)
            for (int i = 0; i < 8; ++i)
                sum += log2(max(luma(texture(picture, vec2(0.08 + 0.12 * float(i), 0.1 + 0.2 * float(j))).rgb), 1e-3));
        float avg = exp2(sum / 40.0);                         // geometric mean of 40 points
        float target_stops = clamp(log2(max(auto_target, 1e-3) / avg), -2.5, 2.5);
        float stops = target_stops;
        float speed = p.values[1].w;                           // 0..1 per frame: how fast the eye adapts (1: at once)
        if (p.has_history > 0.5 && speed < 0.999) {
            float a = texelFetch(history, ivec2(0, 0), 0).a;   // last frame's exposure, stored in the alpha of the picture
            if (a < 0.9) stops = mix((a - 0.5) * 16.0, target_stops, max(speed, 0.001));
        }
        state_alpha = 0.5 + stops / 16.0;
        exposure += stops * auto_strength;
    }
    c *= exp2(exposure);
    vec3 o = c;
    if (curve > 4.5) {
        o = max(c.r, max(c.g, c.b)) > 0.98 ? vec3(1.0, 0.0, 1.0) : c;
    } else if (curve > 3.5) {   // identity below `knee`, then an exponential roll off towards 1
        float knee = clamp(shoulder, 0.1, 0.95);
        vec3 over = max(c - knee, 0.0) / (1.0 - knee);
        o = mix(c, vec3(knee) + (1.0 - knee) * (1.0 - exp(-over)), step(vec3(knee), c));
    } else if (curve > 2.5) {
        float w = hable_(vec3(white)).x;
        o = hable_(c * 2.0) / w;
    } else if (curve > 1.5) {
        o = aces(c * 0.6);
    } else if (curve > 0.5) {
        o = c * (1.0 + c / (white * white)) / (1.0 + c);
    }
    float l = luma(o);
    o = max(mix(vec3(l), o, sat), 0.0);
    color = vec4(o, state_alpha);
}
