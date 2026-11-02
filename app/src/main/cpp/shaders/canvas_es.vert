#version 300 es
// PxPaint canvas quad (OpenGL ES 3.0 variant). See canvas.vert for the layout.
uniform highp vec4 uPush[6];
out highp vec2 vUv;

void main() {
    highp vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    highp vec2 p = uPush[2].xy + corner * uPush[2].zw;
    vUv = mix(uPush[3].xy, uPush[3].zw, corner);
    highp vec3 h = vec3(p, 1.0);
    gl_Position = vec4(dot(uPush[0].xyz, h), dot(uPush[1].xyz, h), 0.0, 1.0);
}
