#version 450
// Lens effects on the finished picture (slot 0): chromatic aberration that grows towards the edges, barrel distortion,
// a sharpen and film grain. Reads only the picture.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;
    vec4 values[4];
} p;

float hash(vec2 x) { return fract(sin(dot(x, vec2(12.9898, 78.233))) * 43758.5453); }

void main() {
    float aberration = p.values[0].x;    // 0.0 to 0.01: how far red and blue move apart at the edge
    float barrel = p.values[0].y;        // 0.0 to 0.2: barrel distortion
    float sharpen = p.values[0].z;       // 0.0 to 1.0
    float grain = p.values[0].w;         // 0.0 to 0.1

    vec2 d = uv - 0.5;
    float r2 = dot(d * vec2(p.resolution.x / p.resolution.y, 1.0), d * vec2(p.resolution.x / p.resolution.y, 1.0));
    vec2 q = 0.5 + d * (1.0 + barrel * r2);
    vec2 shift = d * aberration * (0.3 + r2 * 2.0);
    vec3 c = vec3(texture(picture, q + shift).r, texture(picture, q).g, texture(picture, q - shift).b);
    if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) c = vec3(0.0);

    vec2 px = 1.0 / p.resolution;
    vec3 blur = (texture(picture, q + vec2(px.x, 0.0)).rgb + texture(picture, q - vec2(px.x, 0.0)).rgb +
                 texture(picture, q + vec2(0.0, px.y)).rgb + texture(picture, q - vec2(0.0, px.y)).rgb) * 0.25;
    c += (c - blur) * sharpen * 2.0;
    c += (hash(uv * p.resolution + fract(p.time) * 91.3) - 0.5) * grain;
    color = vec4(max(c, vec3(0.0)), 1.0);
}
