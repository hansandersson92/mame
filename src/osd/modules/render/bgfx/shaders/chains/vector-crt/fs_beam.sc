$input v_beam, v_beam_color, v_beam_timing

// license:BSD-3-Clause
// copyright-holders:Hans Andersson
// Rasterizes each vector as an HDR Gaussian core and broad spot tail with
// scan-order timing into the excitation accumulation buffer. With generator
// timing, intensity controls deposited energy and explicitly redistributes
// high-current energy into the broad PSF component. Untimed generators retain
// the legacy intensity-shaped profile for compatibility.

#include "common.sh"
#include "beam_profile.sh"

#define SQRT_TWO_PI                 2.50662827463
#define TWO_PI                      6.28318530718

#define MIN_PROFILE_INTEGRAL        0.000001
#define MIN_PERSISTENCE             0.001
#define SEGMENT_EPSILON             0.0001

#define SCAN_PERSISTENCE_FRAMES     10.0

// x = frame interval (seconds), y = phosphor persistence (seconds),
// z = beam-energy gain, w = untimed compatibility halo strength.
uniform vec4 u_vector_params;
uniform vec4 u_target_dims;
// x = complete display-list duration (seconds), y = calibrated beam-energy rate,
// z = duration-energy enable, w = maximum timed beam-tail energy fraction.
uniform vec4 u_vector_timing;

// The integral of a Gaussian distance field around a finite segment is the
// sum of its swept-line body and two half-Gaussian endpoint caps. Comparing
// this integral before and after approximate pixel filtering lets the widened
// response preserve energy continuously from stationary dots to long beams.
float capsule_integral(float sigma, float beamLength)
{
	return
		SQRT_TWO_PI * sigma * beamLength +
		TWO_PI * sigma * sigma;
}

vec2 beam_components_filtered(float along, float across, float beamLength, float coreSigma, float broadSigma)
{
	// Outside either endpoint, include longitudinal distance to produce round
	// caps. Between the endpoints only perpendicular distance contributes.
	float pastEndpoint = max(max(-along, along - beamLength), 0.0);
	float pixelAcross = length(vec2(dFdx(across), dFdy(across)));
	float pixelVariance = pixelAcross * pixelAcross / 12.0;
	float filteredCoreSigma = sqrt(coreSigma * coreSigma + pixelVariance);
	float filteredBroadSigma = sqrt(broadSigma * broadSigma + pixelVariance);
	// Preserve the complete capsule integral continuously for every segment
	// length. This approaches one-dimensional sigma compensation for a long
	// beam and two-dimensional squared compensation for a stationary spot.
	float coreScale =
		capsule_integral(coreSigma, beamLength) /
		capsule_integral(filteredCoreSigma, beamLength);
	float broadScale =
		capsule_integral(broadSigma, beamLength) /
		capsule_integral(filteredBroadSigma, beamLength);
	float distanceSquared = across * across + pastEndpoint * pastEndpoint;

	return vec2(
		coreScale * exp(-0.5 * distanceSquared / (filteredCoreSigma * filteredCoreSigma)),
		broadScale * exp(-0.5 * distanceSquared / (filteredBroadSigma * filteredBroadSigma)));
}

void main()
{
	float along = v_beam.x;
	float across = v_beam.y;
	float beamLength = v_beam.z;

	float intensity = max(v_beam_color.a, 0.0);
	float timingEnabled = clamp(u_vector_timing.z, 0.0, 1.0);

	float baseSigma = max(v_beam.w, BEAM_MIN_SIGMA);

	float coreSigma = beam_core_sigma(baseSigma, intensity, timingEnabled);
	float broadSigma = beam_broad_sigma(baseSigma, intensity, timingEnabled);

	vec2 components = beam_components_filtered(
		along,
		across,
		beamLength,
		coreSigma,
		broadSigma);

	float legacyHaloStrength = legacy_beam_halo_strength(
		u_vector_params.w,
		intensity);
	float legacyRadial =
		components.x +
		legacyHaloStrength * components.y;

	float coreIntegral = max(
		capsule_integral(coreSigma, beamLength),
		MIN_PROFILE_INTEGRAL);
	float broadIntegral = max(
		capsule_integral(broadSigma, beamLength),
		MIN_PROFILE_INTEGRAL);
	float tailFraction = timed_beam_tail_fraction(
		u_vector_timing.w,
		intensity);
	float timedRadial =
		(1.0 - tailFraction) * components.x / coreIntegral +
		tailFraction * components.y / broadIntegral;
	float radial = mix(legacyRadial, timedRadial, timingEnabled);

	float durationSeconds =
		max(v_beam_timing.w, 0.0) *
		max(u_vector_timing.x, 0.0);

	float durationResponse =
		durationSeconds *
		u_vector_timing.y;

	float energyResponse =
		mix(
			1.0,
			durationResponse,
			timingEnabled);

	float segmentPosition =
		beamLength > SEGMENT_EPSILON
			? clamp(along / beamLength, 0.0, 1.0)
			: 0.5;

	float arrival = clamp(
		v_beam_timing.x +
		v_beam_timing.y * segmentPosition,
		0.0,
		1.0);

	float age =
		max(
			u_vector_timing.x *
			(1.0 - arrival),
			0.0);

	float scanPersistence =
		max(
			u_vector_params.y,
			u_vector_params.x *
			SCAN_PERSISTENCE_FRAMES);

	float temporal =
		exp(
			-age /
			max(
				scanPersistence,
				MIN_PERSISTENCE));

	vec3 energy =
		v_beam_color.rgb *
		intensity *
		energyResponse *
		radial *
		temporal *
		u_vector_params.z;

	gl_FragColor = vec4(energy, 1.0);
}
