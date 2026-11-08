#version 450
// Colour grading with a lookup table: the PNG next to this shader (postfx/lut_grade.png, a 32x32x32 table laid out as 32
// squares side by side, 1024 x 32) is bound at binding 7. The finished picture (slot 0) is plain RGB.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;
layout(binding = 7) uniform sampler2D lut;
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;
    vec4 values[4];
    float has_history;
    float has_texture;
    vec2 texture_size;
} p;

vec3 lookup(vec3 c) {
    const float N = 32.0;
    c = clamp(c, 0.0, 1.0);
    float slice = c.b * (N - 1.0);
    float s0 = floor(slice), s1 = min(s0 + 1.0, N - 1.0), f = slice - s0;
    // sample at the centres of the texels so the table's end points are exact
    vec2 xy = vec2(c.r * (N - 1.0) + 0.5, c.g * (N - 1.0) + 0.5);
    vec3 a = texture(lut, vec2((s0 * N + xy.x) / (N * N), xy.y / N)).rgb;
    vec3 b = texture(lut, vec2((s1 * N + xy.x) / (N * N), xy.y / N)).rgb;
    return mix(a, b, f);
}

void main() {
    vec3 c = texture(picture, uv).rgb;
    float amount = p.values[0].x;       // 0 off .. 1 full
    if (p.has_texture < 0.5) { color = vec4(c, 1.0); return; }
    color = vec4(mix(c, lookup(c), amount), 1.0);
}
