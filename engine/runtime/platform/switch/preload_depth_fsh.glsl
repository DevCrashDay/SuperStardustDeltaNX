#version 460
layout(binding = 0) uniform sampler2D z;
void main() { gl_FragDepth = texelFetch(z, ivec2(gl_FragCoord.xy), 0).r; }
