#version 450
// A wet road: the asphalt is damp everywhere (darker, glossy, a soft blurred reflection) and has puddles where a noise field
// in the world says so (dark, nearly a mirror, a sharp reflection). The ground is found from the normals that the depth gives,
// turned into the world with the camera's axes; the reflected ray is marched through the depth buffer and what it hits is
// blended in with a Fresnel weight, the blur growing with the roughness and the distance the ray travelled.
// The script passes the camera's right and forward axes and position (see main.lua).
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
float hash21(vec2 p) { p = fract(p * vec2(0.1031, 0.1030)); p += dot(p, p.yx + 33.33); return fract((p.x + p.y) * p.x); }
float vnoise(vec2 x) {
    vec2 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 x) {
    float v = 0.0, a = 0.5;
    for (int k = 0; k < 4; ++k) { v += a * vnoise(x); x = x * 2.03 + 17.3; a *= 0.5; }
    return v;
}
float ign(vec2 x) { return fract(52.9829189 * fract(dot(x, vec2(0.06711056, 0.00583715)))); }

void main() {
    vec3 c = texture(picture, uv).rgb;
    float z = texture(linear_depth, uv).r;
    if (p.has_depth < 0.5 || z > SKY) { color = vec4(c, 1.0); return; }
    vec3 right = p.values[0].xyz;
    vec3 fwd = vec3(p.values[0].w, p.values[1].xy);
    vec3 cpos = vec3(p.values[1].zw, p.values[2].x);
    float wetness = p.values[2].y;          // 0 dry .. 1 soaked: how much of the road is covered by puddles
    float strength = p.values[2].z;         // reflection strength
    float darken = p.values[2].w;           // the darkening of damp asphalt (1 = none)
    float damp_rough = p.values[3].x;       // blur of the reflection on damp asphalt

    vec2 px = 1.0 / vec2(textureSize(linear_depth, 0));
    vec3 P0 = view_pos(uv);
    vec3 X = view_pos(uv + vec2(px.x, 0.0)) - P0, Y = view_pos(uv + vec2(0.0, px.y)) - P0;
    vec3 X2 = P0 - view_pos(uv - vec2(px.x, 0.0)), Y2 = P0 - view_pos(uv - vec2(0.0, px.y));
    if (length(X2) < length(X)) X = X2;
    if (length(Y2) < length(Y)) Y = Y2;
    vec3 N = normalize(cross(X, Y));
    if (dot(N, P0) > 0.0) N = -N;
    // the normal in the world: view x is the right axis, view y the camera's up, view -z the forward axis
    vec3 cam_up = normalize(cross(-fwd, right));
    vec3 Nw = right * N.x + cam_up * N.y + fwd * (-N.z);
    float flat_ground = smoothstep(0.93, 0.99, Nw.y);

    // only dark, grey ground is asphalt: sand and gravel are brighter and more colourful
    float lum = dot(c, vec3(0.0722, 0.7152, 0.2126));
    float sat = (max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b))) / (max(c.r, max(c.g, c.b)) + 1e-3);
    // asphalt is dark and nearly grey: sand and gravel are warmer (the red channel is .b here) and a little brighter
    float warm = (c.b - c.r) / (c.b + c.r + 1e-3);
    flat_ground *= (1.0 - smoothstep(0.10, 0.24, lum)) * (1.0 - smoothstep(0.08, 0.22, sat)) * (1.0 - smoothstep(0.07, 0.17, warm));
    if (flat_ground < 0.01 || wetness < 0.01) { color = vec4(c, 1.0); return; }

    // where the puddles are: noise over the ground, fixed in the world
    vec3 dir_unnorm = right * ray_at(uv).x + cam_up * ray_at(uv).y + fwd;
    vec3 Pw = cpos + normalize(dir_unnorm) * (z * length(dir_unnorm));
    float field = fbm(Pw.xz * 0.16) * 0.7 + fbm(Pw.xz * 0.55 + 31.0) * 0.3;
    float threshold = mix(0.78, 0.40, wetness);
    float puddle = smoothstep(threshold, threshold + 0.07, field);       // 1 inside a puddle, soft edge
    float k = flat_ground;

    // reflection: march the reflected ray through the depth
    vec3 V = normalize(P0);
    vec3 R = reflect(V, N);
    float cos_i = clamp(dot(-V, N), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - cos_i, 5.0);
    float rough = mix(damp_rough, 0.04, puddle);
    float jitter = ign(uv * p.resolution);   // stable in time: the anti-aliasing of the game averages a moving pattern into a smear
    vec3 refl = vec3(0.0);
    float hit = 0.0;
    vec3 pos = P0 + N * 0.03;
    float len = 0.5;
    for (int i = 0; i < 36; ++i) {
        pos += R * len * (0.7 + 0.3 * jitter);
        len *= 1.13;
        vec2 q = project(pos);
        if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0 || pos.z > -0.05) break;
        float sz = texture(linear_depth, q).r;
        float diff = -pos.z - sz;
        if (diff > 0.0 && diff < 1.5 + 0.05 * sz) {
            float edge = smoothstep(0.0, 0.08, min(min(q.x, 1.0 - q.x), min(q.y, 1.0 - q.y)));
            float travelled = length(pos - P0);
            float radius = (1.5 + travelled * 3.0) * rough * 6.0 + 0.7;
            vec3 acc = texture(picture, q).rgb;
            for (int s = 0; s < 6; ++s) {
                float a = 6.2831853 * (float(s) + jitter) / 6.0;
                acc += texture(picture, q + vec2(cos(a), sin(a)) * px * radius * (0.35 + 0.65 * fract(jitter * 11.0 + float(s) * 0.41))).rgb;
            }
            refl = acc / 7.0;
            refl = mix(refl, texture(picture, q).rgb, 0.0);
            hit = edge;
            break;
        }
    }
    if (hit < 0.5) {   // the ray went off screen or into the sky: use the sky picture where the ray points
        vec2 q = project(P0 + R * 400.0);
        vec2 sq = clamp(q, vec2(0.02), vec2(0.98));
        if (R.z < 0.0 && texture(linear_depth, sq).r > SKY) {
            refl = texture(picture, sq).rgb;
        } else {
            refl = texture(picture, vec2(clamp(uv.x, 0.05, 0.95), 0.05)).rgb;
        }
        hit = 0.7;
    }
    // damp asphalt: darker and glossy; puddle: much darker underneath, the reflection carries the look
    float dark = mix(mix(1.0, darken, 0.8), darken * 0.8, puddle);
    vec3 base = c * mix(1.0, dark, k);
    float weight = mix(0.35, 1.0, puddle) * fresnel * strength * hit * k;
    vec3 outc = mix(base, refl, clamp(weight * 1.8, 0.0, 0.85));
    // a puddle also catches the light as a soft specular sheen towards the sun side of the sky
    color = vec4(outc, 1.0);
}
