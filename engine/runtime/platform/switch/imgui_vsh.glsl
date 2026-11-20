#version 460

layout (location = 0) in vec2 inPos;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec4 inCol;

layout (std140, binding = 0) uniform ImGuiUbo {
	mat4 proj;
};

layout (location = 0) out vec2 outUV;
layout (location = 1) out vec4 outCol;

void main() {
	gl_Position = proj * vec4(inPos, 0.0, 1.0);
	outUV = inUV;
	outCol = inCol;
}
