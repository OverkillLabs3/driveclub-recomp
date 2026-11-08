#version 450
// A post-processing filter for dcmod.postfx: a vignette and a warm tint.
// Compile: glslangValidator -V vignette.frag -o files/postfx/vignette.spv
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float pad;
    vec4 values[4];   // the 16 numbers of dcmod.postfx.param(0..15)
} p;

void main() {
    vec3 c = texture(picture, uv).rgb;
    float strength = p.values[0].x;   // param 0: how dark the corners get (0 to 1)
    float warm = p.values[0].y;       // param 1: warm tint (0 to 1)
    vec2 d = (uv - 0.5) * vec2(1.0, p.resolution.y / p.resolution.x) * 1.6;
    c *= 1.0 - strength * smoothstep(0.25, 0.85, length(d));
    c = mix(c, c * vec3(1.15, 1.0, 0.8), warm);
    color = vec4(c, 1.0);
}
