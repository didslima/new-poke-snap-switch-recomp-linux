#version 450
// SPDX-License-Identifier: MIT
// Specular sparkle filter (nvn_motionaa.cpp), Fx pre-tonemap slot, linear HDR scene colour at render resolution, before blended draws.
// A pixel whose luminance is above k times the SECOND brightest of its eight neighbours (plus a small floor) is scaled down to that limit.
// Isolated one- and two-pixel sparks (aliased specular highlights, single sky pixels through a canopy) settle; edges, one-pixel lines (each
// line pixel has two bright neighbours) and highlights wider than two pixels keep their value. Nothing is blurred: a pixel is either
// unchanged or only its brightness is lowered, hue kept.
layout(set = 0, binding = 0) uniform sampler2D src;   // a copy of the scene colour
layout(push_constant) uniform P {
    vec4 q;   // x: k (ratio to the second brightest neighbour), y: floor (linear), z: debug (1 = paint changed pixels magenta), w: unused
} pc;
layout(location = 0) out vec4 o;

float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 hi = textureSize(src, 0) - 1;
    vec4 c = texelFetch(src, p, 0);
    float l = lum(c.rgb);
    float m1 = 0.0, m2 = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) continue;
            float n = lum(texelFetch(src, clamp(p + ivec2(x, y), ivec2(0), hi), 0).rgb);
            m2 = max(m2, min(m1, n));
            m1 = max(m1, n);
        }
    float lim = m2 * pc.q.x + pc.q.y;
    if (l > lim && l > 0.0) {
        c.rgb *= lim / l;
        if (pc.q.z > 0.5) c.rgb = vec3(4.0, 0.0, 4.0);
    }
    o = c;
}
