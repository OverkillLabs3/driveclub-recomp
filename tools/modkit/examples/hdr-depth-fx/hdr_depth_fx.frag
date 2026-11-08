#version 450
// Depth effects on the linear HDR image, before the game's own bloom and tone mapping: distance fog, ambient occlusion,
// depth of field. Reads the view-space distance of every pixel (binding 1) and the view ray parameters (binding 4).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D picture;       // linear HDR
layout(binding = 1) uniform sampler2D linear_depth;  // view-space distance in metres, the sky is 1e6
layout(binding = 2) uniform sampler2D motion;        // pixels, previous minus current
layout(binding = 3) uniform sampler2D gbuffer;       // RGBA8, alpha holds the object number
layout(binding = 4) uniform sampler2D params;        // 4 by 1: frame constants 76..79, 92..95, camera 118, 119
layout(push_constant) uniform P {
    vec2 resolution;
    float time;
    float has_depth;      // 1 when binding 1 to 4 hold the scene, 0 on the finished picture
    vec4 values[4];
} p;

const float SKY = 1.0e5;

vec2 ray_at(vec2 q) {
    vec4 a = texelFetch(params, ivec2(0, 0), 0);
    vec4 b = texelFetch(params, ivec2(1, 0), 0);
    return vec2((a.x * q.x + a.z) * b.z + b.x, (a.y * q.y + a.w) * b.w + b.y);
}
// view-space position of the pixel at q (right handed, forward is -z)
vec3 view_pos(vec2 q) {
    float z = texture(linear_depth, q).r;
    vec2 r = ray_at(q);
    return vec3(z * r, -z);
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
    float z = texture(linear_depth, uv).r;
    bool sky = z > SKY;

    float fog_density = p.values[0].x, fog_strength = p.values[0].y, fog_sky = p.values[0].z, debug_mode = p.values[0].w;
    vec3 fog_color = p.values[1].bgr;   // blue is .r at the HDR stage
    float fog_bright = p.values[1].w;
    float ao_strength = p.values[2].x, ao_radius = p.values[2].y, dof_strength = p.values[2].z, focus_scale = p.values[2].w;
    float focus_dist = p.values[3].x;

    if (debug_mode > 0.5 && debug_mode < 1.5) {
        float d = sky ? 1.0 : clamp(z / 200.0, 0.0, 1.0);
        color = vec4(vec3(d), 1.0);
        return;
    }
    if (debug_mode > 1.5) {   // the normals seen from the depth
        vec2 px = 1.0 / vec2(textureSize(linear_depth, 0));
        vec3 P = view_pos(uv), X = view_pos(uv + vec2(px.x, 0)) - P, Y = view_pos(uv + vec2(0, px.y)) - P;
        vec3 N = normalize(cross(X, Y));
        color = vec4(sky ? vec3(0.0) : N * 0.5 + 0.5, 1.0);
        return;
    }

    // ambient occlusion: look at the surface around the pixel (a disc of taps, sized in metres), and darken where the
    // neighbours rise above the tangent plane of the pixel
    if (!sky && ao_strength > 0.0) {
        vec2 px = 1.0 / vec2(textureSize(linear_depth, 0));
        vec3 P = view_pos(uv);
        vec3 Pr = view_pos(uv + vec2(px.x, 0)), Pl = view_pos(uv - vec2(px.x, 0));
        vec3 Pu = view_pos(uv + vec2(0, px.y)), Pd = view_pos(uv - vec2(0, px.y));
        vec3 X = abs(Pr.z - P.z) < abs(Pl.z - P.z) ? Pr - P : P - Pl;
        vec3 Y = abs(Pu.z - P.z) < abs(Pd.z - P.z) ? Pu - P : P - Pd;
        vec3 N = normalize(cross(X, Y));
        if (dot(N, P) > 0.0) N = -N;
        vec4 a = texelFetch(params, ivec2(0, 0), 0);
        vec4 b = texelFetch(params, ivec2(1, 0), 0);
        vec2 per_metre = 1.0 / (max(z, 0.5) * abs(vec2(a.x * b.z, a.y * b.w)));   // screen units for one metre at this distance
        vec2 reach = clamp(per_metre * ao_radius, px * 2.0, vec2(0.06));
        float angle = hash(uv * p.resolution) * 6.2831853;
        float occ = 0.0;
        for (int i = 0; i < 16; ++i) {
            float t = (float(i) + 0.5) / 16.0;
            float ang = angle + float(i) * 2.399963;
            vec2 q = uv + vec2(cos(ang), sin(ang)) * sqrt(t) * reach;
            vec3 V = view_pos(q) - P;
            float len = length(V);
            float facing = dot(N, V) / max(len, 1e-4);
            // only what is near enough counts, and the further it is the less it counts
            occ += max(facing - 0.12, 0.0) * (1.0 - smoothstep(ao_radius * 0.5, ao_radius * 2.0, len));
        }
        c *= 1.0 - clamp(ao_strength * occ / 16.0 * 2.2, 0.0, 0.9);
    }

    // depth of field: a disc of taps, the blur grows with the distance from the focus
    if (dof_strength > 0.0) {
        float fd = focus_dist > 0.0 ? focus_dist : texture(linear_depth, vec2(0.5, 0.6)).r;
        float zz = sky ? 400.0 : z;
        float coc = clamp(abs(zz - fd) / max(zz, 1.0) * dof_strength * focus_scale, 0.0, 1.0);
        float radius = coc * 0.012;
        if (radius > 0.0004) {
            vec3 sum = c;
            float wsum = 1.0;
            float rot = hash(uv * p.resolution) * 6.2831853;
            for (int i = 0; i < 24; ++i) {
                float a = rot + float(i) * 2.399963;
                float r = sqrt((float(i) + 0.5) / 24.0) * radius;
                vec2 q = uv + vec2(cos(a), sin(a) * p.resolution.x / p.resolution.y) * r;
                float sz = texture(linear_depth, q).r;
                // do not blur a much nearer object over a far one (a tolerance in proportion to the distance, so a road seen
                // at a grazing angle is not mistaken for one)
                float ww = sz > SKY ? 1.0 : clamp((sz / max(z, 0.1) - 0.6) / 0.3, 0.1, 1.0);
                sum += texture(picture, q).rgb * ww;
                wsum += ww;
            }
            c = sum / wsum;
        }
    }

    // fog: the further, the more of the colour of the sky above (so the fog always fits the weather and the hour),
    // tinted by the fog colour; the sky itself gets its own amount
    vec3 sky_here = texture(picture, vec2(uv.x, 0.10)).rgb;
    vec3 fog_col = sky_here * fog_color * fog_bright;
    float f = sky ? fog_sky : (1.0 - exp(-fog_density * z));
    c = mix(c, fog_col, clamp(f * fog_strength, 0.0, 1.0));
    color = vec4(max(c, vec3(0.0)), 1.0);
}
