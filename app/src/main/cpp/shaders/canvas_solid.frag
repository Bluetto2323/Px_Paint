#version 450
// Solid / checkerboard canvas quad.
//   mode 0: solid v[4];  mode 1: screen-space checkerboard of v[4] and v[3].
layout(push_constant) uniform Push { vec4 v[6]; } pc;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    if (pc.v[5].x > 0.5) {
        vec2 cell = floor(gl_FragCoord.xy / max(pc.v[5].y, 1.0));
        outColor = mod(cell.x + cell.y, 2.0) < 0.5 ? pc.v[4] : pc.v[3];
    } else {
        outColor = pc.v[4];
    }
}
