#version 450
// Textured canvas quad: premultiplied texel * tint.
layout(push_constant) uniform Push { vec4 v[6]; } pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTex, vUv) * pc.v[4];
}
