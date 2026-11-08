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

void main() {
    vec3 c = texture(picture, uv).rgb;
    if (p.has_depth < 0.5 || texture(linear_depth, uv).r < 1.0e5) { color = vec4(c, 1.0); return; }
    vec3 sun = normalize(p.values[0].xyz);           // towards the sun, view space (x right, y up, -z forward)
    float keep = p.values[0].w;                      // how much of the game's own sky stays
    vec3 up = normalize(p.values[1].xyz);            // the world's up in view space
    float disc = p.values[1].w;                      // size of the sun disc (0.0005 is small, 0.003 large)
    vec3 sun_colour = p.values[2].rgb;               // the sun's colour (normalised), black at night
    float glow = p.values[2].w;                      // strength of the glow round the sun
    float haze = p.values[3].x;                      // 0 clear, 1 hazy (a wider pale band at the horizon)
    float brightness = p.values[3].y;                // the sky's brightness in the scene's units (0.5 is about white)

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
    sky += sun_colour * brightness * 12.0 * smoothstep(1.0 - disc, 1.0 - disc * 0.4, cosg) * step(0.0, height + 0.02);
    color = vec4(mix(sky.bgr, c, keep), 1.0);   // blue is .r at the HDR stage
}
