#version 300 es
// Solid / checkerboard canvas quad (OpenGL ES 3.0). See canvas_solid.frag.
precision highp float;
uniform highp vec4 uPush[6];
in highp vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    if (uPush[5].x > 0.5) {
        vec2 cell = floor(gl_FragCoord.xy / max(uPush[5].y, 1.0));
        outColor = mod(cell.x + cell.y, 2.0) < 0.5 ? uPush[4] : uPush[3];
    } else {
        outColor = uPush[4];
    }
}
