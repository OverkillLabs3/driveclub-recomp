#version 450
// Thick volumetric fog. Every view ray is marched through a real 3D density field in world space: ground fog that thins
// out with height, broken up by drifting 3D noise into banks and clearings. At each step the sun is tested through the fog
// itself (self shadowing: the lit side of a bank glows, the inside and the far side are dark and cool), the sky gives the
// ambient colour, and a phase function brightens the fog towards the sun. What is behind the fog is dimmed by the
// transmittance, so distant things disappear into it the way they do in the real thing.
// Needs the camera place and axes and the sun direction in the WORLD (dcmod.camera, dcmod.sun): see main.lua.
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
// integer style hash and 3D value noise (stable at large coordinates)
float hash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}
float noise3(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i), hash13(i + vec3(1, 0, 0)), f.x), mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x), mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm3(vec3 x) {
    float v = 0.0, a = 0.5;
    for (int k = 0; k < 3; ++k) { v += a * noise3(x); x = x * 2.07 + 11.7; a *= 0.5; }
    return v / 0.875;
}

vec3 cam_pos, cam_right, cam_up, cam_fwd;
float ground, density0, falloff, thick;

// fog density at a world point, per metre
float density(vec3 w) {
    float h = max(w.y - ground, 0.0);
    float base = density0 * exp(-falloff * h);
    if (base < 1e-5) return 0.0;
    vec3 drift = vec3(p.time * 0.35, 0.0, p.time * 0.15);                 // the wind carries the fog along
    float big = fbm3(w * 0.011 + drift * 0.15);                           // large patches: thick here, clear there
    float fine = fbm3(w * 0.055 + drift);                                 // billows inside the patches
    float shape = smoothstep(0.30, 0.62, big) * (0.35 + 0.95 * smoothstep(0.28, 0.72, fine));
    return base * (0.04 + 2.8 * shape) * thick;
}

vec2 project(vec3 v) {
    vec4 a = texelFetch(params, ivec2(0, 0), 0);
    vec4 b = texelFetch(params, ivec2(1, 0), 0);
    vec2 r = v.xy / max(-v.z, 1e-4);
    return vec2(((r.x - b.x) / b.z - a.z) / a.x, ((r.y - b.y) / b.w - a.w) / a.y);
}
// the same field with only the large patches: what the sun taps and the shadow test need (one noise lookup, not six)
float density_light(vec3 w) {
    float h = max(w.y - ground, 0.0);
    float base = density0 * exp(-falloff * h);
    if (base < 1e-5) return 0.0;
    vec3 drift = vec3(p.time * 0.35, 0.0, p.time * 0.15);
    float big = noise3(w * 0.011 * 1.0 + drift * 0.15) * 0.65 + noise3(w * 0.022 + drift * 0.15) * 0.35;
    return base * (0.04 + 2.8 * smoothstep(0.30, 0.62, big) * 0.65) * thick;
}
float ign(vec2 x) { return fract(52.9829189 * fract(dot(x, vec2(0.06711056, 0.00583715)))); }

void main() {
    vec3 c = texture(picture, uv).rgb;
    if (p.has_depth < 0.5) { color = vec4(c, 1.0); return; }
    cam_right = vec3(p.values[0].xyz);
    cam_fwd   = vec3(p.values[0].w, p.values[1].xy);
    cam_pos   = vec3(p.values[1].zw, p.values[2].x);
    vec3 sun  = normalize(vec3(p.values[2].yzw));                         // towards the sun, world
    density0  = p.values[3].x;
    falloff   = p.values[3].y;
    ground    = p.values[3].z;
    thick     = p.values[3].w;
    cam_up = normalize(cross(-cam_fwd, cam_right));

    vec2 r = ray_at(uv);
    vec3 dir_unnorm = cam_right * r.x + cam_up * r.y + cam_fwd;           // world direction for a view depth of 1
    float len = length(dir_unnorm);
    vec3 dir = dir_unnorm / len;

    float z = texture(linear_depth, uv).r;
    const float MAX_DIST = 260.0;
    float dist_surface = (z > SKY ? 1.0e4 : z) * len;                     // metres along the ray to the surface
    float dist = min(dist_surface, MAX_DIST);
    // above this height the density is below what could be seen: do not march rays up into empty air
    float h_top = log(max(density0 * 3.0 * thick, 1e-5) / 0.0006) / max(falloff, 1e-3);
    float cam_h = cam_pos.y - ground;
    if (dir.y > 1e-3) dist = min(dist, max((h_top - cam_h) / dir.y, 0.0));

    // the sky above gives the ambient light: the average of the open sky along the top of the picture (game channel order)
    vec3 sky_sum = vec3(0.0);
    float sky_w = 0.0;
    for (int j = 0; j < 1; ++j)
        for (int i = 0; i < 4; ++i) {
            vec2 sq = vec2(0.15 + 0.23 * float(i), 0.05);
            float open = texture(linear_depth, sq).r > SKY ? 1.0 : 0.0;
            sky_sum += texture(picture, sq).rgb * open;
            sky_w += open;
        }
    vec3 ambient = sky_w > 0.5 ? sky_sum / sky_w : vec3(0.35, 0.30, 0.25);
    ambient = mix(ambient, vec3(dot(ambient, vec3(0.333))), 0.12);        // fog scatters, it greys the sky colour a little

    float day = smoothstep(-0.06, 0.22, sun.y);
    float elev = clamp(sun.y, 0.0, 1.0);
    vec3 sun_col = mix(vec3(1.0, 0.55, 0.28), vec3(1.0, 0.93, 0.80), smoothstep(0.0, 0.5, elev)).bgr * day;
    float cosg = dot(dir, sun);
    const float g1 = 0.72, g2 = -0.25;
    float phase = mix((1.0 - g2 * g2) / pow(1.0 + g2 * g2 - 2.0 * g2 * cosg, 1.5),
                      (1.0 - g1 * g1) / pow(1.0 + g1 * g1 - 2.0 * g1 * cosg, 1.5), 0.55) / 12.566;

    const int STEPS = 18;
    float jitter = ign(uv * p.resolution + vec2(p.time * 37.0, p.time * 17.0));
    float trans = 1.0;
    vec3 insc = vec3(0.0);
    float t_prev = 0.0;
    for (int i = 0; i < STEPS; ++i) {
        float f = (float(i) + jitter) / float(STEPS);
        float t = dist * f * f;                                           // finer steps near the camera
        float dt = t - t_prev;
        t_prev = t;
        vec3 w = cam_pos + dir * t;
        float d = density(w);
        if (d < 1e-5) continue;
        // the sun through the fog: two taps towards the sun give the optical depth that shadows this point
        float od = density_light(w + sun * 8.0) * 8.0 + density_light(w + sun * 30.0) * 30.0;
        float sun_vis = exp(-od * 0.65);
        // things standing in the picture between the fog and the sun (palms, pylons, grandstands) cast shafts into it
        {
            vec3 Pv = vec3(r.x, r.y, -1.0) * (t / len);
            vec3 sv = vec3(dot(sun, cam_right), dot(sun, cam_up), -dot(sun, cam_fwd));
            for (int s = 1; s <= 4; ++s) {
                vec3 Q = Pv + sv * (float(s * s) * 1.3 + jitter * 1.5);
                vec2 q = project(Q);
                if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0 || Q.z > -0.2) break;
                float sz = texture(linear_depth, q).r;
                if (sz < -Q.z - 0.2 && sz < SKY) { sun_vis *= 0.12; break; }
            }
        }
        float h = max(w.y - ground, 0.0);
        float sky_vis = exp(-h * falloff * 0.5);                          // fog under the bank sees less sky
        // shadowed fog is cool and dim (it only sees the sky), lit fog is warm and bright; the phase function adds a glow
        // towards the sun that is strongest on the edges of a bank
        vec3 shade = ambient * vec3(1.18, 1.0, 0.80) * (0.40 + 0.34 * (1.0 - sky_vis));
        vec3 light = shade + sun_col * sun_vis * (0.22 + phase * 4.2) * 1.15;
        float step_t = exp(-d * dt);
        insc += trans * (1.0 - step_t) * light;
        trans *= step_t;
        if (trans < 0.02) break;
    }
    color = vec4(c * trans + insc, 1.0);
}
