#version 450
// One shader, four jobs, chosen by values[0].x, so that a chain of slots makes a bloom with an anamorphic glare:
//   0  bright pass and first downsample of the scene        (slot at half size)
//   1  blur and downsample                                  (slots at a quarter and an eighth)
//   2  upsample with a tent filter                          (slot at a quarter)
//   3  composite: the scene plus the bloom and the glare    (slot at full size)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;   // what the slot before drew (the scene for the first slot)
layout(binding = 5) uniform sampler2D scene;     // the HDR picture before the first HDR slot
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;
    vec4 values[4];
} p;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    int mode = int(p.values[0].x + 0.5);
    vec2 px = 1.0 / vec2(textureSize(picture, 0));
    if (mode == 0) {
        // 13 taps of the scene around the pixel, only what is brighter than the threshold counts
        float threshold = p.values[0].y, knee = p.values[0].z;
        vec3 sum = vec3(0.0);
        float wsum = 0.0;
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                vec2 o = vec2(x, y) * px * 0.9;
                vec3 c = texture(picture, uv + o).rgb;
                float l = luma(c);
                float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
                soft = soft * soft / (4.0 * knee + 1e-5);
                float contribution = max(soft, l - threshold) / max(l, 1e-4);
                float w = exp(-0.35 * float(x * x + y * y));
                sum += c * contribution * w;
                wsum += w;
            }
        }
        color = vec4(sum / wsum, 1.0);
    } else if (mode == 1) {
        vec3 sum = vec3(0.0);
        float wsum = 0.0;
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                float w = exp(-0.3 * float(x * x + y * y));
                sum += texture(picture, uv + vec2(x, y) * px * 1.3).rgb * w;
                wsum += w;
            }
        }
        color = vec4(sum / wsum, 1.0);
    } else if (mode == 2) {
        vec3 sum = texture(picture, uv).rgb * 4.0;
        sum += texture(picture, uv + vec2(px.x, 0)).rgb * 2.0;
        sum += texture(picture, uv - vec2(px.x, 0)).rgb * 2.0;
        sum += texture(picture, uv + vec2(0, px.y)).rgb * 2.0;
        sum += texture(picture, uv - vec2(0, px.y)).rgb * 2.0;
        sum += texture(picture, uv + px).rgb;
        sum += texture(picture, uv - px).rgb;
        sum += texture(picture, uv + vec2(px.x, -px.y)).rgb;
        sum += texture(picture, uv + vec2(-px.x, px.y)).rgb;
        color = vec4(sum / 16.0, 1.0);
    } else {
        float strength = p.values[0].y, streak = p.values[0].z, streak_len = p.values[0].w;
        float dirt = p.values[1].x;
        vec3 s = texture(scene, uv).rgb;
        vec3 bloom = texture(picture, uv).rgb;
        // anamorphic glare: the bloom smeared along the horizontal, fading out
        vec3 glare = vec3(0.0);
        float gsum = 0.0;
        for (int i = -12; i <= 12; ++i) {
            float w = exp(-abs(float(i)) * 0.22);
            glare += texture(picture, uv + vec2(float(i) * px.x * streak_len, 0.0)).rgb * w;
            gsum += w;
        }
        glare /= gsum;
        vec3 tint = vec3(0.75, 0.85, 1.25);   // the cool tint of a lens streak
        color = vec4(s + bloom * strength + glare * streak * tint, 1.0);
    }
}
