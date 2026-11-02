#version 450
// PxPaint canvas quad (Vulkan variant, compiled to SPIR-V by tools/compile_shaders.sh).
// One quad per draw, generated from gl_VertexIndex (triangle strip, 4 vertices).
// Push constants (96 bytes, layout shared with canvas_es.vert):
//   v[0], v[1] : rows of the canvas -> NDC affine transform (GL convention, y up)
//   v[2]       : quad rect in canvas pixels (x, y, w, h)
//   v[3]       : textured: uv rect (u0, v0, u1, v1); solid: second color
//   v[4]       : color (premultiplied): texture tint / solid color
//   v[5]       : params (mode, checker cell in px, 0, 0)
layout(push_constant) uniform Push { vec4 v[6]; } pc;
layout(location = 0) out vec2 vUv;

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 p = pc.v[2].xy + corner * pc.v[2].zw;
    vUv = mix(pc.v[3].xy, pc.v[3].zw, corner);
    vec3 h = vec3(p, 1.0);
    // Vulkan clip space has +Y down: same flip as the Block 1 triangle.
    gl_Position = vec4(dot(pc.v[0].xyz, h), -dot(pc.v[1].xyz, h), 0.0, 1.0);
}
