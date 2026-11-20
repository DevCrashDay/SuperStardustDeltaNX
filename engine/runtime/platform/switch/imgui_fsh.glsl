#version 460

layout (binding = 0) uniform sampler2D uTex;

layout (location = 0) in vec2 inUV;
layout (location = 1) in vec4 inCol;

layout (location = 0) out vec4 outCol;

void main() {
	outCol = inCol * texture(uTex, inUV);
}
