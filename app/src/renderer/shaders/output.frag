#version 450
// Output pass of the Vulkan 1.1 renderer: the game's front buffer, one texel per output pixel, through the gamma
// ramp the game loads (the Xbox 360 display applies it after scan-out) and the optional post-processing.
//
// Specialization constants:
//   0, kGrading: saturation, vibrance, gamma, vignette and scanlines done per pixel (otherwise the single-channel
//      post-processing is already folded into the ramp table, see Output::UpdateRampTable).
//   1, kFxaa: FXAA at normal quality before the ramp (nfsmw_antialiasing = fxaa).
//   2, kRcas: AMD FidelityFX FSR1 RCAS sharpening before the ramp (nfsmw_rcas). Not combined with FXAA: with both,
//      FXAA is used.
// Every constant comes from the uniform buffer: no push constants.
//
// The headers are generated with:
//   glslangValidator -V --target-env vulkan1.1 --vn output_fs -o output_fs.h output.frag
//   glslangValidator -V --target-env vulkan1.1 --vn output_vs -o output_vs.h output.vert

layout(constant_id = 0) const bool kGrading = false;
layout(constant_id = 1) const bool kFxaa = false;
layout(constant_id = 2) const bool kRcas = false;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1, std140) uniform Constants {
  vec4 ramp[256];     // red, green and blue of each entry (10-bit value / 1023, with single-channel effects)
  vec4 grading;       // saturation, vibrance, 1 / gamma (kGrading)
  vec4 effects;       // vignette, scanlines (kGrading), RCAS sharpness as a linear factor (kRcas)
  vec4 texel;         // 1 / source width, 1 / source height
} constants;

layout(location = 0) out vec4 color;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

float Luma(vec2 uv) {
  return dot(textureLod(source, uv, 0.0).rgb, kLumaWeights);
}

// FXAA at normal quality (edge threshold 0.166, minimum 0.0833, 8-texel reach), as in the SDK.
vec3 Fxaa(vec2 uv) {
  vec2 step = constants.texel.xy;
  vec3 center = textureLod(source, uv, 0.0).rgb;
  float luma_center = dot(center, kLumaWeights);
  float luma_nw = Luma(uv + vec2(-step.x, -step.y));
  float luma_ne = Luma(uv + vec2(step.x, -step.y));
  float luma_sw = Luma(uv + vec2(-step.x, step.y));
  float luma_se = Luma(uv + vec2(step.x, step.y));
  float luma_min = min(luma_center, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
  float luma_max = max(luma_center, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
  if (luma_max - luma_min < max(0.0833, luma_max * 0.166)) {
    return center;
  }
  vec2 direction = vec2(-((luma_nw + luma_ne) - (luma_sw + luma_se)), (luma_nw + luma_sw) - (luma_ne + luma_se));
  float reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * (0.25 * 0.125), 1.0 / 128.0);
  float inverse_min = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
  direction = clamp(direction * inverse_min, vec2(-8.0), vec2(8.0)) * step;
  vec3 rgb_a = 0.5 * (textureLod(source, uv + direction * (1.0 / 3.0 - 0.5), 0.0).rgb +
                      textureLod(source, uv + direction * (2.0 / 3.0 - 0.5), 0.0).rgb);
  vec3 rgb_b = rgb_a * 0.5 + 0.25 * (textureLod(source, uv + direction * -0.5, 0.0).rgb +
                                     textureLod(source, uv + direction * 0.5, 0.0).rgb);
  float luma_b = dot(rgb_b, kLumaWeights);
  return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

// FSR1 RCAS (ffx_fsr1.h, FsrRcasF, without the noise removal): a five-tap cross sharpened with the largest negative
// lobe that keeps the result inside the neighbours' range, so it does not ring or clip.
const float kRcasLimit = 0.25 - (1.0 / 16.0);
vec3 Rcas(ivec2 pixel) {
  ivec2 last = textureSize(source, 0) - 1;
  vec3 b = texelFetch(source, clamp(pixel + ivec2(0, -1), ivec2(0), last), 0).rgb;
  vec3 d = texelFetch(source, clamp(pixel + ivec2(-1, 0), ivec2(0), last), 0).rgb;
  vec3 e = texelFetch(source, pixel, 0).rgb;
  vec3 f = texelFetch(source, clamp(pixel + ivec2(1, 0), ivec2(0), last), 0).rgb;
  vec3 h = texelFetch(source, clamp(pixel + ivec2(0, 1), ivec2(0), last), 0).rgb;
  vec3 min4 = min(min(b, d), min(f, h));
  vec3 max4 = max(max(b, d), max(f, h));
  const vec2 peak = vec2(1.0, -4.0);
  vec3 hit_min = min(min4, e) / (4.0 * max4);
  vec3 hit_max = (peak.x - max(max4, e)) / (4.0 * min4 + peak.y);
  vec3 lobe_rgb = max(-hit_min, hit_max);
  float lobe = max(-kRcasLimit, min(max(lobe_rgb.r, max(lobe_rgb.g, lobe_rgb.b)), 0.0)) * constants.effects.z;
  return (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  vec3 texel;
  if (kFxaa) {
    texel = Fxaa((vec2(pixel) + 0.5) * constants.texel.xy);
  } else if (kRcas) {
    texel = clamp(Rcas(pixel), 0.0, 1.0);
  } else {
    texel = texelFetch(source, pixel, 0).rgb;
  }
  uvec3 i = uvec3(texel * 255.0 + 0.5);
  vec3 c = vec3(constants.ramp[i.r].r, constants.ramp[i.g].g, constants.ramp[i.b].b);
  if (kGrading) {
    // Saturation around luma, and vibrance (boosts the least saturated pixels more), then gamma.
    float luma = dot(max(c, 0.0), vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, constants.grading.x);
    float saturation = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = mix(vec3(luma), c, 1.0 + constants.grading.y * (1.0 - clamp(saturation, 0.0, 1.0)));
    c = pow(max(c, vec3(0.0001)), vec3(constants.grading.z));
    // Vignette: four bands of 42 % from each edge, which add up in the corners.
    vec2 uv = (vec2(pixel) + 0.5) * constants.texel.xy;
    float v = constants.effects.x;
    c *= (1.0 - v * max(0.0, 1.0 - uv.y / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.y) / 0.42)) *
         (1.0 - v * max(0.0, 1.0 - uv.x / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.x) / 0.42));
    // Scanlines: one dark row out of every three.
    if ((uint(pixel.y) % 3u) == 0u) {
      c *= 1.0 - constants.effects.y * 0.7;
    }
    c = clamp(c, 0.0, 1.0);
  }
  color = vec4(c, 1.0);
}
