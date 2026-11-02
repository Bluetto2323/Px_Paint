#version 300 es
// Textured canvas quad (OpenGL ES 3.0). highp: uv must address 2048+ texels exactly.
precision highp float;
uniform highp vec4 uPush[6];
uniform highp sampler2D uTex;
in highp vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTex, vUv) * uPush[4];
}
