#version 450
// Ambient occlusion and one bounce of light from the screen (screen space global illumination). Per pixel a few rays are
// marched over the hemisphere above the surface (the normals come from the depth); where a ray meets something that stands
// in front, the pixel is darkened and takes a little of that something's colour. The occlusion is smoothed over time with
// the history binding and the motion vectors: its running value lives in the alpha of the picture.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;
layout(binding = 1) uniform sampler2D linear_depth;
layout(binding = 2) uniform sampler2D motion;
layout(binding = 3) uniform sampler2D gbuffer;
layout(binding = 4) uniform sampler2D params;
layout(binding = 5) uniform sampler2D scene;
layout(binding = 6) uniform sampler2D history;
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;
    vec4 values[4];
    float has_history;
} p;
const float SKY = 1.0e5;

vec2 ray_at(vec2 q) {
    vec4 a = texelFetch(params, ivec2(0, 0), 0);
    vec4 b = texelFetch(params, ivec2(1, 0), 0);
    return vec2((a.x * q.x + a.z) * b.z + b.x, (a.y * q.y + a.w) * b.w + b.y);
}
vec3 view_pos(vec2 q) { float z = texture(linear_depth, q).r; return vec3(z * ray_at(q), -z); }
vec2 project(vec3 v) {
    vec4 a = texelFetch(params, ivec2(0, 0), 0);
    vec4 b = texelFetch(params, ivec2(1, 0), 0);
    vec2 r = v.xy / max(-v.z, 1e-4);
    return vec2(((r.x - b.x) / b.z - a.z) / a.x, ((r.y - b.y) / b.w - a.w) / a.y);
}
float hash(vec2 x) { return fract(sin(dot(x, vec2(12.9898, 78.233))) * 43758.5453); }

void main() {
    vec3 c = texture(picture, uv).rgb;
    float z = texture(linear_depth, uv).r;
    if (p.has_depth < 0.5 || z > SKY) { color = vec4(c, 1.0); return; }
    float radius = p.values[0].x;       // metres
    float ao_strength = p.values[0].y;  // 0..1
    float bounce = p.values[0].z;       // strength of the colour taken from the surroundings
    float blend = p.values[0].w;        // how much of the new frame goes into the running value (0.1 smooth, 1 no smoothing)

    vec2 px = 1.0 / vec2(textureSize(linear_depth, 0));
    vec3 P0 = view_pos(uv);
    vec3 X = view_pos(uv + vec2(px.x, 0.0)) - P0, Y = view_pos(uv + vec2(0.0, px.y)) - P0;
    vec3 X2 = P0 - view_pos(uv - vec2(px.x, 0.0)), Y2 = P0 - view_pos(uv - vec2(0.0, px.y));
    if (length(X2) < length(X)) X = X2;
    if (length(Y2) < length(Y)) Y = Y2;
    vec3 N = normalize(cross(X, Y));
    if (dot(N, P0) > 0.0) N = -N;

    float seed = hash(uv * p.resolution + fract(p.time * 0.37) * 17.0);
    vec3 T = normalize(abs(N.y) < 0.99 ? cross(N, vec3(0, 1, 0)) : cross(N, vec3(1, 0, 0)));
    vec3 B = cross(N, T);
    float occ = 0.0, wsum = 0.0;
    vec3 gather = vec3(0.0);
    const int DIRS = 6, STEPS = 4;
    for (int i = 0; i < DIRS; ++i) {
        float a = 6.2831853 * (float(i) + seed) / float(DIRS);
        float elev = mix(0.25, 1.0, fract(seed * 7.13 + float(i) * 0.618));
        vec3 dir = normalize(T * cos(a) * (1.0 - elev * 0.5) + B * sin(a) * (1.0 - elev * 0.5) + N * elev);
        for (int s = 1; s <= STEPS; ++s) {
            float t = radius * (float(s) / float(STEPS)) * (0.5 + 0.5 * fract(seed * 3.7 + float(s)));
            vec3 Sp = P0 + dir * t;
            vec2 q = project(Sp);
            if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) break;
            float sz = texture(linear_depth, q).r;
            float diff = -Sp.z - sz;             // > 0: the sample point is behind a surface
            if (diff > 0.03 && diff < radius * 1.5) {
                float w = 1.0 - float(s - 1) / float(STEPS);
                occ += w;
                gather += texture(picture, q).rgb * w;
                break;
            }
        }
        wsum += 1.0;
    }
    float ao_now = 1.0 - clamp(occ / wsum, 0.0, 1.0);
    vec3 bleed_now = gather / max(occ, 1e-3);

    float ao = ao_now;
    if (p.has_history > 0.5) {                   // the running value, taken from where this pixel was a frame ago
        vec2 prev_uv = uv + texture(motion, uv).rg / p.resolution;
        if (prev_uv.x > 0.0 && prev_uv.y > 0.0 && prev_uv.x < 1.0 && prev_uv.y < 1.0) {
            float pa = texture(history, prev_uv).a;
            // the old value may not stray far from what this frame sees, or a thing that moved leaves a ghost behind
            if (pa < 0.99) ao = mix(clamp(pa, ao_now - 0.2, ao_now + 0.2), ao_now, clamp(blend, 0.02, 1.0));
        }
    }
    float shade = mix(1.0, ao, ao_strength);
    vec3 lit = c * shade;
    lit += bleed_now * (1.0 - ao_now) * bounce * c;      // light that bounced off the neighbours, tinted by this surface
    color = vec4(max(lit, vec3(0.0)), min(ao, 0.98));
}
