// license:BSD-3-Clause
// copyright-holders:Hans Andersson
//
// Physical model terminology:
//
// intensity
//   Normalized vector-generator Z level. In the energy model it represents
//   relative beam current: greater intensity means a greater rate of energy
//   deposition while the beam is enabled.
//
// rampDuration
//   Time taken for the X/Y deflection system to move from the vector start
//   position to the end position. It describes beam traversal speed and is
//   not necessarily equal to beamOnDuration.
//
// beamOnDuration
//   Time for which the electron beam is enabled (Z on) for the vector
//   operation. This is exposure time, independent of the distance travelled
//   by the beam.
//
// depositedEnergy
//   Relative energy deposited by the beam during the operation. The model
//   computes this from vector intensity and the time for which the beam is on:
//
//       depositedEnergy proportional to intensity * beamOnDuration
//
//   This is a relative excitation quantity.
//
// spatialProfile
//   Normalized core-plus-halo distribution describing where deposited energy
//   lands. Its integral is normalized so changing beam width redistributes
//   energy spatially rather than creating or destroying energy.
//
// excitation
//   Deposited phosphor energy density at a particular location. This is the
//   persistent quantity accumulated by the phosphor simulation before decay
//   and conversion to emitted light.
//
// emission
//   Visible light produced from the stored phosphor excitation. This is
//   distinct from deposited energy and may have a nonlinear response at high
//   excitation.
//
// In short:
//
//   rampDuration   -> how quickly the beam moves through space
//   beamOnDuration -> how long beam current is present
//
// baseSigma
//   Standard deviation of the narrow Gaussian component of the spatial beam
//   profile. It represents the effective core width of the electron-beam
//   exposure at the phosphor surface. It controls spatial distribution, not
//   the amount of deposited energy.
//
// haloSigma
//   Standard deviation of the broad Gaussian component of the spatial beam
//   profile. It represents the wider, low-density component surrounding the
//   beam core. It is derived from baseSigma by a fixed monitor-profile scale
//   and does not depend on beam intensity in the duration-based physical model.
//
// haloStrength
//   Relative weight of the broad Gaussian component compared with the core.
//   Together with haloSigma and normalization, it determines how deposited
//   energy is distributed between the narrow core and broad halo.
//
// Spatial beam-profile constants and width functions shared by the geometry
// and fragment stages. Keep quad coverage and evaluated response in lockstep.

#define BEAM_MIN_SIGMA                   0.01
#define BEAM_PIXEL_VARIANCE              (1.0 / 12.0)
#define BEAM_HALO_BASE_WIDTH_SCALE       3.5
#define LEGACY_BEAM_CORE_WIDTH_GAIN      0.12
#define LEGACY_BEAM_HALO_WIDTH_GAIN      0.25
#define LEGACY_BEAM_HALO_MIN_STRENGTH    0.6

// Untimed generators cannot derive deposited energy from beam-on duration, so
// retain the former visual compensation. The square root is deliberately
// concave: it gives low Z levels more width and halo response than a linear
// mapping (for example, 0.25 maps to 0.5). This kept dim vectors and dots
// visible when their dwell energy was unavailable. It is compatibility
// behavior, not part of the duration-based deposited-energy model.
float legacy_beam_intensity_response(float intensity)
{
	return sqrt(clamp(intensity, 0.0, 1.0));
}

float beam_core_sigma(float baseSigma, float intensity, float timingEnabled)
{
	float response = legacy_beam_intensity_response(intensity);
	float legacySigma =
		baseSigma *
		(1.0 + LEGACY_BEAM_CORE_WIDTH_GAIN * response);
	return mix(legacySigma, baseSigma, clamp(timingEnabled, 0.0, 1.0));
}

float beam_halo_sigma(float baseSigma, float intensity, float timingEnabled)
{
	float response = legacy_beam_intensity_response(intensity);
	float physicalSigma = baseSigma * BEAM_HALO_BASE_WIDTH_SCALE;
	float legacySigma =
		physicalSigma *
		(1.0 + LEGACY_BEAM_HALO_WIDTH_GAIN * response);
	return mix(legacySigma, physicalSigma, clamp(timingEnabled, 0.0, 1.0));
}

float beam_halo_strength(float configuredStrength, float intensity, float timingEnabled)
{
	float response = legacy_beam_intensity_response(intensity);
	float legacyStrength =
		configuredStrength *
		mix(LEGACY_BEAM_HALO_MIN_STRENGTH, 1.0, response);
	return mix(
		legacyStrength,
		configuredStrength,
		clamp(timingEnabled, 0.0, 1.0));
}
