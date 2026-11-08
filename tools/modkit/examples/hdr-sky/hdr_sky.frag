#version 450
// A physically based sky painted where the depth says sky: single scattering through an atmosphere with Rayleigh (blue sky,
// red sunsets) and Mie (haze and the glow round the sun) layers, ray marched from the camera, with a clouds layer lit by the
// same sun. The script gives the sun and the camera's axes (see main.lua); the colours come out of the physics, not out of
// a gradient: the sky is deep blue overhead, pale at the horizon, orange at a low sun, dark at night.
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

vec2 ray_at(vec2 q) {
    vec4 a = texelFetch(params, ivec2(0, 0), 0);
    vec4 b = texelFetch(params, ivec2(1, 0), 0);
    return vec2((a.x * q.x + a.z) * b.z + b.x, (a.y * q.y + a.w) * b.w + b.y);
}
float hash21(vec2 p) { p = fract(p * vec2(0.1031, 0.1030)); p += dot(p, p.yx + 33.33); return fract((p.x + p.y) * p.x); }
float vnoise(vec2 x) {
    vec2 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 x) {
    float v = 0.0, a = 0.5;
    for (int k = 0; k < 5; ++k) { v += a * vnoise(x); x = x * 2.03 + 17.3; a *= 0.5; }
    return v;
}

const float R_EARTH = 6371.0e3, R_ATMO = 6471.0e3;     // metres
const vec3 BETA_R = vec3(5.8e-6, 13.5e-6, 33.1e-6);    // Rayleigh scattering, per metre, red green blue
const float BETA_M = 21.0e-6;                           // Mie scattering
const float H_R = 8000.0, H_M = 1200.0;

// distance along a ray from a point at radius r (a height above the ground) to the top of the atmosphere
float atmo_exit(float r, float mu) {
    float b = r * mu;
    return -b + sqrt(max(b * b - r * r + R_ATMO * R_ATMO, 0.0));
}

void main() {
    vec3 c = texture(picture, uv).rgb;
    if (p.has_depth < 0.5 || texture(linear_depth, uv).r < 1.0e5) { color = vec4(c, 1.0); return; }
    vec3 sun_v = normalize(p.values[0].xyz);          // towards the sun, view space (x right, y up, -z forward)
    float coverage = p.values[0].w;                   // cloud cover 0 .. 1
    vec3 up = normalize(p.values[1].xyz);             // the world's up in view space
    vec2 right_xz = vec2(p.values[1].w, p.values[2].w), fwd_xz = vec2(p.values[3].x, p.values[3].y), up_xz = vec2(p.values[3].z, p.values[3].w);
    float sun_power = clamp(length(p.values[2].xyz) * 1.4, 0.0, 1.0);   // 0 at night, 1 by day (the script passes the sun's colour)

    vec3 d = normalize(vec3(ray_at(uv), -1.0));       // the view ray
    float mu = dot(d, up);
    // a frame with y up in which the ray lies in the xy plane and the sun has its own azimuth
    vec3 hx = d - mu * up;
    hx = length(hx) > 1e-4 ? normalize(hx) : normalize(cross(up, vec3(0.0, 0.0, 1.0)));
    vec3 hz = cross(hx, up);
    vec3 S = vec3(dot(sun_v, hx), dot(sun_v, up), dot(sun_v, hz));    // the sun in that frame
    vec3 D = vec3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
    float cosg = dot(D, S);

    // the camera is a little above the ground; below the horizon the ground would hide the sky: look at the horizon instead
    float mu_s = max(mu, 0.0015);
    D = vec3(sqrt(1.0 - mu_s * mu_s), mu_s, 0.0);
    float r0 = R_EARTH + 2.0;
    float t_max = atmo_exit(r0, mu_s);

    const int N = 12, NL = 5;
    float ds = t_max / float(N);
    vec3 sum_r = vec3(0.0), sum_m = vec3(0.0);
    float od_r = 0.0, od_m = 0.0;
    for (int i = 0; i < N; ++i) {
        vec3 pos = vec3(0.0, r0, 0.0) + D * (ds * (float(i) + 0.5));
        float h = length(pos) - R_EARTH;
        float hr = exp(-h / H_R) * ds, hm = exp(-h / H_M) * ds;
        od_r += hr; od_m += hm;
        // light reaching this point: march towards the sun
        vec3 up_here = normalize(pos);
        float mu_sun = dot(up_here, S);
        if (mu_sun < -0.12 && length(pos) * sqrt(max(1.0 - mu_sun * mu_sun, 0.0)) < R_EARTH) continue;   // in the earth's shadow
        float t_l = atmo_exit(length(pos), mu_sun);
        float dl = t_l / float(NL), odl_r = 0.0, odl_m = 0.0;
        for (int j = 0; j < NL; ++j) {
            vec3 pl = pos + S * (dl * (float(j) + 0.5));
            float hl = length(pl) - R_EARTH;
            odl_r += exp(-hl / H_R) * dl;
            odl_m += exp(-hl / H_M) * dl;
        }
        vec3 tau = BETA_R * (od_r + odl_r) + 1.11 * BETA_M * (od_m + odl_m);
        vec3 att = exp(-tau);
        sum_r += att * hr;
        sum_m += att * hm;
    }
    // phase functions: Rayleigh and Cunningham-Henyey-Greenstein for Mie
    float phase_r = 3.0 / (16.0 * 3.14159265) * (1.0 + cosg * cosg);
    const float g = 0.76;
    float phase_m = 3.0 / (8.0 * 3.14159265) * ((1.0 - g * g) * (1.0 + cosg * cosg)) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * cosg, 1.5));
    const float SUN_I = 22.0;
    vec3 sky = SUN_I * (sum_r * BETA_R * phase_r + sum_m * BETA_M * phase_m);
    // the part of the day when the sun is far under the horizon: a dim, cool night sky
    vec3 night = mix(vec3(0.010, 0.016, 0.034), vec3(0.002, 0.004, 0.012), clamp(mu, 0.0, 1.0));
    sky = sky * 0.55 + night * (1.0 - sun_power);
    // the sun itself
    float sun_disc = smoothstep(0.99985, 0.99998, cosg) * step(0.0, mu);
    vec3 transmit = exp(-(BETA_R * od_r + 1.11 * BETA_M * od_m));
    sky += transmit * sun_disc * 40.0 * sun_power;

    // clouds: a layer overhead lit by the same sun, tinted by the sky's own light
    if (mu > 0.01 && coverage > 0.01) {
        vec2 world_xz = d.x * right_xz + d.y * up_xz + (-d.z) * fwd_xz;
        vec2 plane = world_xz / (mu + 0.18) * 0.65 + vec2(p.time * 0.010, p.time * 0.004);
        float dens = fbm(plane);
        float edge = 1.0 - coverage;
        float cloud = smoothstep(edge - 0.04, edge + 0.20, dens);
        vec2 sun_xz = normalize(sun_v.x * right_xz + sun_v.y * up_xz + (-sun_v.z) * fwd_xz + vec2(1e-4));
        float toward = fbm(plane + sun_xz * 0.05);
        float lit = clamp(0.55 + (dens - toward) * 6.0, 0.12, 1.25);
        // sun colour at the cloud: the atmosphere's attenuation along the sun's path (orange at sunset)
        float mus = clamp(S.y, 0.02, 1.0);
        vec3 sun_trans = exp(-(BETA_R * 8000.0 + vec3(1.11 * BETA_M * 1200.0)) / mus * 0.9);
        vec3 cloud_col = (sun_trans * 3.1 * lit * sun_power + vec3(0.10, 0.13, 0.20) * (0.35 + 0.65 * sun_power) + night * 2.0 * (1.0 - sun_power));
        cloud_col *= mix(1.0, 0.62, smoothstep(0.62, 1.0, dens) * 0.6);
        float fade = smoothstep(0.015, 0.16, mu);
        sky = mix(sky, cloud_col * 0.55, cloud * fade * 0.92);
    }
    color = vec4(max(sky, vec3(0.0)).bgr, 1.0);       // blue is .r at the HDR stage
}
