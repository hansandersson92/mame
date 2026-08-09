// license:BSD-3-Clause
// copyright-holders:Hans Andersson
// Converts persistent phosphor excitation to emitted light while preserving
// chromaticity. Both the direct image and bloom use this physical boundary.

vec3 phosphor_emission(vec3 excitation)
{
	excitation = max(excitation, vec3_splat(0.0));
	float luminance = dot(excitation, vec3(0.2126, 0.7152, 0.0722));
	float emittedLuminance = 1.0 - exp(-luminance);
	// The response is linear near zero, so emittedLuminance/luminance tends
	// to one. Clamp only the divisor to avoid an unstable zero division.
	float safeLuminance = max(luminance, 1e-6);
	return excitation * (emittedLuminance / safeLuminance);
}
