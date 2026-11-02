#version 300 es
// OpenGL ES 3.0 variant (embedded as source, compiled by the driver).
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
out vec3 vColor;

void main() {
    vColor = inColor;
    gl_Position = vec4(inPosition, 0.0, 1.0);
}
