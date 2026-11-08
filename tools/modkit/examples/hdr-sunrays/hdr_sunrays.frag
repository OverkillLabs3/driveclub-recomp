#version 450
// Light shafts and lens ghosts of the sun on the linear HDR image, before the game's bloom and tone mapping. The script
// gives the sun's place on the screen (dcmod.sun) in values[0].xy; the shafts are a radial blur of the open sky towards it.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;       // linear HDR
layout(binding = 1) uniform sampler2D linear_depth;  // view-space distance in metres, the sky is 1e6
layout(binding = 2) uniform sampler2D motion;
layout(binding = 3) uniform sampler2D gbuffer;
layout(binding = 4) uniform sampler2D params;
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;
    vec4 values[4];
} p;

void main() {
    vec3 c = texture(picture, uv).rgb;
    if (p.has_depth < 0.5 || p.values[0].z < 0.5) { color = vec4(c, 1.0); return; }
    vec2 sun = p.values[0].xy;
    float strength = p.values[0].w;                  // shafts, scaled by how bright the sun is
    vec3 tint = p.values[1].rgb;
    float decay = p.values[1].w;                     // 0.9 to 0.99: how far the shafts reach
    float length_scale = p.values[2].x;              // 0..1 of the way to the sun that is sampled
    float ghosts = p.values[2].y;                    // lens ghost strength
    float aspect = p.resolution.x / p.resolution.y;

    const int N = 48;
    vec2 delta = (sun - uv) * length_scale / float(N);
    vec2 q = uv;
    float weight = 1.0, shaft = 0.0;
    for (int i = 0; i < N; ++i) {
        q += delta;
        if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) break;
        float open = texture(linear_depth, q).r > 1.0e5 ? 1.0 : 0.0;
        shaft += open * weight;
        weight *= decay;
    }
    shaft /= float(N) * 0.35;
    float near_sun = exp(-length((uv - sun) * vec2(aspect, 1.0)) * 1.6);
    vec3 rays = tint * shaft * near_sun * strength;

    // ghosts: soft discs mirrored through the middle of the picture
    vec3 g = vec3(0.0);
    vec2 axis = vec2(0.5) - sun;
    for (int i = 1; i <= 4; ++i) {
        vec2 centre = sun + axis * (0.55 * float(i));
        float d = length((uv - centre) * vec2(aspect, 1.0));
        float r = 0.035 + 0.02 * float(i);
        float disc = smoothstep(r, r * 0.55, d) * (1.0 - smoothstep(0.0, 0.5, length(sun - vec2(0.5)) * 0.8));
        g += disc * mix(vec3(1.0, 0.55, 0.3), vec3(0.3, 0.6, 1.0), float(i) / 4.0) * (0.35 / float(i));
    }
    g *= ghosts * strength;
    color = vec4(c + rays + g, 1.0);
}
