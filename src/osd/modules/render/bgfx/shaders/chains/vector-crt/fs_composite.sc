$input v_texcoord0

// license:BSD-3-Clause
// copyright-holders:Hans Andersson
// Combines persistent excitation and emitted-light bloom, then applies exposure, luminance
// tone mapping, gamma correction, and optional edge vignetting for display.

#include "common.sh"
#include "phosphor_emission.sh"

SAMPLER2D(s_accum, 0);
SAMPLER2D(s_bloom, 1);
uniform vec4 u_composite;
uniform vec4 u_phosphor_model;

#define DISPLAY_TONE_MAP_KNEE 0.75

// Preserve scene-linear luminance ratios through the ordinary operating
// range, introducing a smooth shoulder only near the SDR output ceiling.
float tone_map_luminance(float luminance)
{
	return
		luminance <= DISPLAY_TONE_MAP_KNEE
			? luminance
			: DISPLAY_TONE_MAP_KNEE +
				(1.0 - DISPLAY_TONE_MAP_KNEE) *
				(1.0 - exp(
					-(luminance - DISPLAY_TONE_MAP_KNEE) /
					(1.0 - DISPLAY_TONE_MAP_KNEE)));
}

// Convert emitted monochrome P4 phosphor light to the renderer's linear-sRGB
// working space using its nominal blue-white chromaticity.
vec3 p4_to_linear_srgb(vec3 phosphor)
{
	const vec3 P4_WHITE = vec3(0.805, 1.014, 1.445);
	return phosphor * P4_WHITE;
}

// Provisional generic P22 phosphor conversion, derived from MAME's existing
// HLSL chromaticity defaults:
//
//   R (0.630, 0.340), G (0.310, 0.595), B (0.155, 0.070)
//   Y (0.2124, 0.7011, 0.0866)
//
// This is a period-appropriate P22 approximation, not a measurement of an
// individual tube. Neutral excitation remains effectively neutral, while the
// emitted-light phosphor channels are converted to the renderer's linear-sRGB
// working space before display tone mapping and gamma correction.
vec3 p22_to_linear_srgb(vec3 phosphor)
{
	mat3 transform = mtxFromRows3(
			vec3( 0.939540,  0.050179, 0.010236),
			vec3( 0.017873,  0.965850, 0.016440),
			vec3(-0.001599, -0.004357, 1.006451));
	return mul(transform, phosphor);
}

void main()
{
	vec3 phosphor = phosphor_emission(
			texture2D(s_accum, v_texcoord0).rgb);

	vec3 bloom = max(
			texture2D(s_bloom, v_texcoord0).rgb,
			vec3_splat(0.0));

	vec3 emitted = phosphor + bloom * u_composite.x;
	vec3 monitorColor;
	if (u_phosphor_model.x > 0.5)
		monitorColor = p4_to_linear_srgb(emitted);
	else
		monitorColor = p22_to_linear_srgb(emitted);
	vec3 hdr = monitorColor * u_composite.y;

	// Tone-map luminance rather than each RGB channel separately.
	// This retains the saturation and hue of bright vector colors.
	float luminance = dot(
			hdr,
			vec3(0.2126, 0.7152, 0.0722));

	float mappedLuminance = mix(
			1.0 - exp(-luminance),
			tone_map_luminance(luminance),
			clamp(u_phosphor_model.y, 0.0, 1.0));

	vec3 mapped =
			luminance > 0.0001
			? hdr * (mappedLuminance / luminance)
			: vec3_splat(0.0);

	mapped = pow(
			max(mapped, vec3_splat(0.0)),
			vec3_splat(
					1.0 /
					max(u_composite.z, 0.001)));

	vec2 edge =
			v_texcoord0 *
			(vec2_splat(1.0) - v_texcoord0);

	float vignette =
			clamp(
					16.0 * edge.x * edge.y,
					0.0,
					1.0);

	mapped *= mix(
			1.0,
			vignette,
			u_composite.w);

	gl_FragColor = vec4(mapped, 1.0);
}
