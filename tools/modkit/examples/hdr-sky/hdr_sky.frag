#version 450
// A sky of its own on the linear HDR image: a gradient from the horizon to the zenith, a sun disc and a glow round it,
// painted where the depth says "sky" (the scenery, the car and the HUD are untouched). The script gives the sun's direction
// in view space and the colours; the game's own sky (with its clouds) is mixed back in by the amount in values[0].w.
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

void main() {
    vec3 c = texture(picture, uv).rgb;
    if (p.has_depth < 0.5 || texture(linear_depth, uv).r < 1.0e5) { color = vec4(c, 1.0); return; }
    vec3 sun = normalize(p.values[0].xyz);           // towards the sun, view space (x right, y up, -z forward)
    float coverage = p.values[0].w;                  // 0 clear sky .. 1 overcast
    vec3 up = normalize(p.values[1].xyz);            // the world's up in view space
    vec3 sun_colour = p.values[2].rgb;               // the sun's colour (normalised), black at night
    // the camera's right, forward and up seen from above (x and z of each), to turn a view ray into a world direction
    vec2 right_xz = vec2(p.values[1].w, p.values[2].w), fwd_xz = vec2(p.values[3].x, p.values[3].y), up_xz = vec2(p.values[3].z, p.values[3].w);
    const float disc = 0.0016, glow = 1.0, haze = 0.4, brightness = 0.45, keep = 0.08;

    vec3 d = normalize(vec3(ray_at(uv), -1.0));      // the view ray
    float height = dot(d, up);
    float elev = dot(sun, up);                       // the sun's height: 1 overhead, 0 on the horizon, below 0 at night
    float day = smoothstep(-0.12, 0.25, elev);
    vec3 zenith = mix(vec3(0.004, 0.008, 0.03), vec3(0.10, 0.27, 0.75), day);
    vec3 horizon = mix(vec3(0.01, 0.015, 0.04), mix(vec3(0.95, 0.5, 0.25), vec3(0.62, 0.78, 1.0), smoothstep(0.0, 0.45, elev)), day);
    float band = pow(clamp(1.0 - abs(height), 0.0, 1.0), mix(5.0, 2.0, haze));
    vec3 sky = mix(zenith, horizon, band) * brightness * 2.0;
    float cosg = max(dot(d, sun), 0.0);
    sky += sun_colour * brightness * (pow(cosg, 6.0) * glow * 0.25 + pow(cosg, 48.0) * glow * 0.6);

    // clouds: a layer overhead, seen in perspective, drifting with the wind, lit from the side the sun is on
    if (height > 0.01 && coverage > 0.01) {
        vec2 world_xz = d.x * right_xz + d.y * up_xz + (-d.z) * fwd_xz;      // the ray's direction over the ground
        vec2 plane = world_xz / (height + 0.2) * 0.7 + vec2(p.time * 0.012, p.time * 0.004);
        float dens = fbm(plane);
        float edge = 1.0 - coverage;
        float cloud = smoothstep(edge - 0.05, edge + 0.22, dens);
        // light: the cloud is brighter where the density falls off towards the sun
        vec2 sun_xz = normalize(sun.x * right_xz + sun.y * up_xz + (-sun.z) * fwd_xz + 1e-4);
        float toward = fbm(plane + sun_xz * 0.06);
        float lit = clamp(0.65 + (dens - toward) * 5.0, 0.15, 1.35);
        vec3 lightc = mix(vec3(0.75, 0.82, 1.0), mix(vec3(1.0, 0.62, 0.38), vec3(1.0), smoothstep(0.0, 0.5, elev)), day);
        vec3 cloud_col = lightc * lit * mix(0.03, 1.0, day) * brightness * 2.2;
        cloud_col = mix(cloud_col, cloud_col * 0.55, smoothstep(0.6, 1.0, dens) * 0.5);   // dense cores are greyer
        float fade = smoothstep(0.01, 0.18, height);      // thin out into the haze at the horizon
        sky = mix(sky, cloud_col, cloud * fade);
    }
    sky += sun_colour * brightness * 12.0 * smoothstep(1.0 - disc, 1.0 - disc * 0.4, cosg) * step(0.0, height + 0.02);
    color = vec4(mix(sky.bgr, c, keep), 1.0);   // blue is .r at the HDR stage
}
