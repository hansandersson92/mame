$input a_position, i_data0, i_data1, i_data2
$output v_beam, v_beam_color, v_beam_timing

// license:BSD-3-Clause
// copyright-holders:Hans Andersson
// Expands each instanced vector segment into a padded screen-space quad and
// passes beam-local distance, color, width, and timing data to the beam shader.

#include "common.sh"
#include "beam_profile.sh"

// z = duration-energy enable; the spatial profile uses it to distinguish the
// duration-based physical model from compatibility rendering.
uniform vec4 u_vector_timing;

void main()
{
	// Per-instance data is packed by beam_instance in vectorrenderer.cpp:
	//   i_data0 = segment endpoints in target-pixel coordinates
	//   i_data1 = linear RGB and Gaussian sigma in pixels
	//   i_data2 = normalized scan start/ramp duration, intensity, beam-on duration
	// Timing contains generator-derived normalized scan start/ramp duration when
	// available. Render-length-derived timing is used only as a compatibility
	// fallback for generators that do not yet provide operation duration.
	vec2 p0 = i_data0.xy;
	vec2 p1 = i_data0.zw;
	vec2 delta = p1 - p0;
	float beamLength = length(delta);
	vec2 direction = beamLength > 0.0001 ? delta / beamLength : vec2(1.0, 0.0);
	vec2 normal = vec2(-direction.y, direction.x);
	float baseSigma = max(i_data1.w, BEAM_MIN_SIGMA);
	float intensity = max(i_data2.z, 0.0);
	float timingEnabled = clamp(u_vector_timing.z, 0.0, 1.0);
	float coreSigma = beam_core_sigma(baseSigma, intensity, timingEnabled);
	float haloSigma = beam_halo_sigma(baseSigma, intensity, timingEnabled);
	float filteredCoreSigma = sqrt(coreSigma * coreSigma + BEAM_PIXEL_VARIANCE);
	float filteredHaloSigma = sqrt(haloSigma * haloSigma + BEAM_PIXEL_VARIANCE);

	// a_position describes a unit segment quad: x selects an endpoint and y is
	// the signed side. Extend it by six times the widest pixel-filtered sigma so
	// neither the core nor halo is clipped by the quad boundary. Beam-local
	// coordinates are target pixels, making the pixel-box variance 1/12 here as
	// in the fragment shader.
	float padding = max(max(filteredCoreSigma, filteredHaloSigma) * 6.0, 1.0);
	float along = mix(-padding, beamLength + padding, a_position.x);
	float across = a_position.y * padding;
	vec2 world = p0 + direction * along + normal * across;

	gl_Position = mul(u_viewProj, vec4(world, 0.0, 1.0));
	// Beam-local coordinates let the fragment shader evaluate distance without
	// depending on target orientation. Timing is constant for the whole instance.
	v_beam = vec4(along, across, beamLength, baseSigma);
	v_beam_color = vec4(i_data1.rgb, i_data2.z);
	v_beam_timing = i_data2;
}
