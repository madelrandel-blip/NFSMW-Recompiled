#version 450
// Copies sample 0 of a multisampled depth render target into a single-sample depth texture (Vulkan 1.1 has no
// depth resolve). The offset moves from destination pixels to source pixels; it comes in a uniform buffer (no push
// constants anywhere in this renderer).
layout(set = 0, binding = 0) uniform sampler2DMS source;
layout(set = 0, binding = 1, std140) uniform Constants {
  ivec2 source_offset;
} constants;

void main() {
  gl_FragDepth = texelFetch(source, ivec2(gl_FragCoord.xy) + constants.source_offset, 0).r;
}
