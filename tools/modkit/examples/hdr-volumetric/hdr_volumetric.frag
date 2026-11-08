#version 450
// Volumetric light: the sunlight scattered by the air along every view ray. Each ray is marched in steps; at every step the
// point is tested against the depth buffer in the direction of the sun (is something between it and the sun?), and the
// light that gets through is added, weighted by how much the air scatters towards the camera (a phase function that peaks
// when looking at the sun). Palms, pylons and the grandstand cut shafts into the haze. The script gives the sun's direction
// in view space and its colour.
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
vec2 project(vec3 v) {
    vec4 a = texelFetch(params, ivec2(0, 0), 0);
    vec4 b = texelFetch(params, ivec2(1, 0), 0);
    vec2 r = v.xy / max(-v.z, 1e-4);
    return vec2(((r.x - b.x) / b.z - a.z) / a.x, ((r.y - b.y) / b.w - a.w) / a.y);
}
float hash(vec2 x) { return fract(sin(dot(x, vec2(12.9898, 78.233))) * 43758.5453); }

void main() {
    vec3 c = texture(picture, uv).rgb;
    if (p.has_depth < 0.5) { color = vec4(c, 1.0); return; }
    vec3 sun = normalize(p.values[0].xyz);       // towards the sun, view space (x right, y up, -z forward)
    float density = p.values[0].w;               // scattering per metre
    vec3 sun_col = p.values[1].bgr;              // the sun's colour (blue is .r at this stage)
    float strength = p.values[1].w;
    float max_dist = p.values[2].x;              // how far along the ray the light is gathered, metres
    float anis = p.values[2].y;                  // 0 even .. 0.9 strongly towards the sun

    float z = texture(linear_depth, uv).r;
    vec3 ray = vec3(ray_at(uv), -1.0);           // not normalised: z along the view axis is 1
    float ray_len = length(ray);
    float t_end = min(z > SKY ? max_dist : z, max_dist);      // view-axis depth where the march stops
    vec3 d = ray / ray_len;
    float cosg = dot(d, sun);
    // Henyey-Greenstein phase function
    float g = anis;
    float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosg, 1.5) / 12.566;

    const int STEPS = 22, SHADOW = 5;
    float jitter = hash(uv * p.resolution + fract(p.time) * 31.0);
    float acc = 0.0;
    float prev = 0.0;
    for (int i = 0; i < STEPS; ++i) {
        float f = (float(i) + jitter) / float(STEPS);
        float t = t_end * f * f;                 // more steps near the camera
        float dt = max(t - prev, 0.0);
        prev = t;
        vec3 P = ray * t;                        // view-space point (z = -t)
        // is the sun reaching this point? walk towards the sun in the picture and look for something in front
        float lit = 1.0;
        for (int s = 1; s <= SHADOW; ++s) {
            vec3 Q = P + sun * (float(s) * 1.4 + 0.5 * jitter);
            vec2 q = project(Q);
            if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0 || Q.z > -0.1) break;
            float sz = texture(linear_depth, q).r;
            if (sz < -Q.z - 0.15 && sz < SKY) { lit = 0.0; break; }
        }
        acc += lit * dt * exp(-density * t * 0.35);
    }
    float day = smoothstep(-0.05, 0.2, sun.y);   // (sun.y is view space: roughly "up" only when level)
    vec3 shafts = sun_col * acc * density * phase * strength * 12.566;
    color = vec4(c * exp(-density * t_end * 0.35) + shafts, 1.0);
}
