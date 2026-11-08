#version 450
// Lens dirt: the bright parts of the picture light up the smudges and specks of a dirty lens (the PNG at binding 7). The glow
// comes from a wide blur of what is brighter than a threshold; the dirt picture decides where it shows.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;
layout(binding = 7) uniform sampler2D dirt;
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;
    vec4 values[4];
    float has_history;
    float has_texture;
    vec2 texture_size;
} p;

float luma(vec3 c) { return dot(c, vec3(0.0722, 0.7152, 0.2126)); }   // blue is .r at the HDR stage

void main() {
    vec3 c = texture(picture, uv).rgb;
    if (p.has_depth < 0.5 || p.has_texture < 0.5) { color = vec4(c, 1.0); return; }
    float strength = p.values[0].x;      // 0 off .. 2
    float threshold = p.values[0].y;     // brightness above which things light the dirt (the scene's white is about 0.5)
    float spread = p.values[0].z;        // how wide the glow is (fraction of the picture)

    // a wide, soft measure of the light around the middle of the picture and around this pixel
    vec3 glow = vec3(0.0);
    float wsum = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -3; i <= 3; ++i) {
            vec2 o = vec2(float(i) / 3.0, float(j) / 2.0) * spread;
            vec3 s = texture(picture, uv + o).rgb;
            float w = 1.0 - 0.35 * length(vec2(i / 3.0, j / 2.0));
            glow += max(s - threshold, 0.0) * w;
            wsum += w;
        }
    }
    glow /= wsum;
    float d = texture(dirt, uv).r;
    d = d * d * 1.6;
    c += glow * d * strength * 6.0;
    color = vec4(c, 1.0);
}
