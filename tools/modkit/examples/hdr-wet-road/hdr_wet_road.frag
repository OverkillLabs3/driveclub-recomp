#version 450
// Screen space reflections on flat ground, for a wet road look: the ground is found from the normals the depth gives,
// the reflected ray is marched through the depth buffer and the picture found there is blended in with a Fresnel
// weight; the road itself is darkened and made glossier. The script gives the world's up in view space in values[0].xyz.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;
layout(binding = 1) uniform sampler2D linear_depth;
layout(binding = 2) uniform sampler2D motion;
layout(binding = 3) uniform sampler2D gbuffer;
layout(binding = 4) uniform sampler2D params;
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;
    vec4 values[4];
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

void main() {
    vec3 c = texture(picture, uv).rgb;
    float z = texture(linear_depth, uv).r;
    if (p.has_depth < 0.5 || z > SKY) { color = vec4(c, 1.0); return; }
    vec3 up = normalize(p.values[0].xyz);
    float wet = p.values[0].w;               // 0 dry .. 1 soaked
    float strength = p.values[1].x;          // reflection strength
    float darken = p.values[1].y;            // how much the wet ground darkens
    float roughness = p.values[1].z;         // 0 mirror .. 1 blurry

    vec2 px = 1.0 / vec2(textureSize(linear_depth, 0));
    vec3 P0 = view_pos(uv);
    vec3 X = view_pos(uv + vec2(px.x, 0.0)) - P0, Y = view_pos(uv + vec2(0.0, px.y)) - P0;
    vec3 N = normalize(cross(X, Y));
    if (dot(N, P0) > 0.0) N = -N;            // face the camera
    float flat_ground = smoothstep(0.90, 0.985, dot(N, up));
    // only dark, grey ground (asphalt) gets wet: sand and gravel are brighter and more colourful
    float lum = dot(c, vec3(0.0722, 0.7152, 0.2126));
    float sat = (max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b))) / (max(c.r, max(c.g, c.b)) + 1e-3);
    flat_ground *= (1.0 - smoothstep(0.10, 0.22, lum)) * (1.0 - smoothstep(0.18, 0.40, sat));
    if (flat_ground < 0.01 || wet < 0.01) { color = vec4(c, 1.0); return; }

    vec3 V = normalize(P0);                  // from the camera to the point
    vec3 R = reflect(V, N);
    float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(-V, N), 0.0, 1.0), 5.0);   // mostly at grazing angles

    vec3 refl = vec3(0.0);
    float hit = 0.0;
    const int STEPS = 40;
    float len = 0.6;
    vec3 pos = P0 + N * 0.02;
    float jitter = fract(sin(dot(uv * p.resolution, vec2(12.9898, 78.233))) * 43758.5453 + p.time);
    for (int i = 0; i < STEPS; ++i) {
        pos += R * len * (0.6 + 0.4 * jitter);
        len *= 1.12;
        vec2 q = project(pos);
        if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0 || pos.z > -0.05) break;
        float sz = texture(linear_depth, q).r;
        float diff = -pos.z - sz;            // positive: the ray is behind the surface
        if (diff > 0.0 && diff < 2.0 + 0.04 * sz) {
            float edge = smoothstep(0.0, 0.1, min(min(q.x, 1.0 - q.x), min(q.y, 1.0 - q.y)));
            // the further the reflected thing, the blurrier (a wet road is glossy, not a mirror)
            float travelled = length(pos - P0);
            float radius = (4.0 + travelled * 3.0) * roughness;
            vec3 acc = vec3(0.0);
            for (int k = 0; k < 8; ++k) {
                float a = 6.2831853 * (float(k) + jitter) / 8.0;
                acc += texture(picture, q + vec2(cos(a), sin(a)) * px * radius * (0.4 + 0.6 * fract(jitter * 13.0 + float(k) * 0.37))).rgb;
            }
            refl = (acc + texture(picture, q).rgb) / 9.0;
            hit = edge;
            break;
        }
        if (-pos.z - sz > 40.0 && sz > SKY) { break; }
    }
    if (hit < 0.5) {                         // the ray left the picture or hit the sky: use the sky above
        vec2 sq = vec2(clamp(uv.x, 0.05, 0.95), 0.06);
        refl = texture(picture, sq).rgb * (texture(linear_depth, sq).r > SKY ? 1.0 : 0.6);
        hit = 0.55;
    }
    float k = flat_ground * wet;
    vec3 base = c * mix(1.0, darken, k);
    vec3 out_c = mix(base, refl, clamp(fresnel * strength * hit * k * 1.6, 0.0, 0.55));
    color = vec4(out_c, 1.0);
}
