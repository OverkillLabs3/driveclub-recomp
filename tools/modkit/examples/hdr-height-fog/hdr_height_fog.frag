#version 450
// Height fog with light scattering: the fog is thicker near the ground, the sun lights it up (a bright haze round the sun
// and a warm tint towards it), and the density along each pixel's ray is worked out exactly from its height and distance.
// The script gives the world's up in view space, the camera's height and the sun's direction and colour.
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

void main() {
    vec3 c = texture(picture, uv).rgb;
    if (p.has_depth < 0.5) { color = vec4(c, 1.0); return; }
    vec3 up = normalize(p.values[0].xyz);          // world up in view space
    float cam_h = p.values[0].w;                   // the camera's height above the ground level of the fog
    vec3 sun = normalize(p.values[1].xyz);         // towards the sun, view space
    float density = p.values[1].w;                 // fog per metre at ground level
    vec3 sun_col = p.values[2].bgr;                // the sun's colour, normalised (blue is .r at this stage)
    float falloff = p.values[2].w;                 // per metre of height: how quickly the fog thins out going up
    float scatter = p.values[3].x;                 // strength of the sun's glow in the fog
    float base = p.values[3].y;                    // brightness of the fog's own colour
    vec3 amb = p.values[3].zzz;                    // unused spare

    float z = texture(linear_depth, uv).r;
    bool sky = z > SKY;
    vec3 d = normalize(vec3(ray_at(uv), -1.0));    // the view ray
    float dist = sky ? 4000.0 : z * length(vec3(ray_at(uv), -1.0));   // distance along the ray in metres
    float dh = dot(d, up);                         // how much of the ray goes up
    // the fog's optical depth along the ray: the integral of density * exp(-falloff * height)
    float k = falloff * dh;
    float depth_int = abs(k) < 1e-4 ? dist : (1.0 - exp(-k * dist)) / k;
    float tau = density * exp(-falloff * cam_h) * depth_int;
    float transmit = exp(-tau);

    float cosg = max(dot(d, sun), 0.0);
    float day = smoothstep(-0.1, 0.25, dot(sun, up));
    vec3 fog_colour = mix(vec3(0.15, 0.2, 0.3), vec3(0.6, 0.72, 0.95), day) * base;   // a pale sky blue by day
    fog_colour = fog_colour.bgr;
    vec3 glow = sun_col * (pow(cosg, 4.0) * 0.5 + pow(cosg, 24.0)) * scatter * day;
    c = c * transmit + (fog_colour + glow) * (1.0 - transmit);
    color = vec4(c, 1.0);
}
