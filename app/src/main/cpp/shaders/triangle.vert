#version 450
// Vulkan variant (compiled to SPIR-V by tools/compile_shaders.sh).
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 vColor;

void main() {
    vColor = inColor;
    // Vulkan clip space has +Y pointing down. Flipping here keeps a single
    // vertex convention for both backends without relying on negative
    // viewport heights (VK_KHR_maintenance1 / Vulkan 1.1 only).
    gl_Position = vec4(inPosition.x, -inPosition.y, 0.0, 1.0);
}
