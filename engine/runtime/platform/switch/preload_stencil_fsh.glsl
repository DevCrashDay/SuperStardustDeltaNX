#version 460
layout(binding = 0) uniform sampler2D st;
layout(std140, binding = 0) uniform Bit { uint bit; } b;
void main() { if ((uint(texelFetch(st, ivec2(gl_FragCoord.xy), 0).r * 255.0 + 0.5) & b.bit) == 0u) discard; }
