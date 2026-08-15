// license:BSD-3-Clause
// copyright-holders:Hans Andersson
//============================================================
//
//  vectorrenderer.cpp - Persistent GPU vector CRT simulation
//
//============================================================

#include "vectorrenderer.h"

#include "effect.h"
#include "effectmanager.h"
#include "uniform.h"
#include "vertex.h"

#include "../frontend/mame/ui/menuitem.h"
#include "../frontend/mame/ui/slider.h"

#include "modules/lib/osdobj_common.h"

#include "emu.h"
#include "osdcore.h"
#include "render.h"
#include "strformat.h"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>


namespace {

constexpr unsigned BLOOM_PASSES = 2;

constexpr float GAUSSIAN_FWHM_TO_SIGMA = 2.354820045f;

// Provisional timed core FWHM at 1080 lines. This is a rendering default, not
// a measured CRT spot size. It scales with target height, so lower-resolution
// targets may have a subpixel core; the shader's pixel-footprint integration
// preserves its energy without silently clamping its physical width.
constexpr float CORE_FWHM_1080 = 1.0f;

// Preserve the previous primitive-width calculation for untimed generators.
// The new width-control default is 1.0, so include the former default of 0.75.
constexpr float LEGACY_UNTIMED_BEAM_WIDTH_SCALE = 0.75f;
constexpr float LEGACY_UNTIMED_BEAM_SIGMA_SCALE = 0.085f;

/*
 * Timed phosphor operating-point calibration at 1080 vertical pixels. A
 * 60-second Asteroids sample measured the median peak core response of moving
 * lines before the global energy rate as 2.72562e-7. Map that reference
 * response to 0.10 pre-emission excitation:
 *
 *   BEAM_ENERGY_RATE_1080 = 0.10 / 2.72562e-7
 *                         = 366889.001402
 *
 * This is a reference operating point for useful phosphor dynamic range, not
 * an absolute luminance measurement of a real CRT.
 */
constexpr double REFERENCE_CORE_RESPONSE_1080 = 2.72562e-7;
constexpr double REFERENCE_PHOSPHOR_EXCITATION = 0.10;
constexpr double BEAM_ENERGY_RATE_1080 =
		REFERENCE_PHOSPHOR_EXCITATION /
		REFERENCE_CORE_RESPONSE_1080;

// Restore the reference moving-line median to approximately RGB 189 while
// retaining a smooth, unclipped output ceiling that quantizes to RGB 255.
constexpr float TIMED_REFERENCE_EXPOSURE = 7.656055f;
constexpr float LEGACY_UNTIMED_EXPOSURE = 0.78f;

constexpr double SQRT_TWO_PI = 2.5066282746310002;
constexpr double TWO_PI = 6.2831853071795865;

// Temporary game-name lookup for testing monitor-family beam-current gains.
// This is deliberately not the final monitor-profile architecture.
struct timed_beam_gain_entry
{
	char const *game;
	float gain;
};

constexpr timed_beam_gain_entry TIMED_BEAM_GAIN_TESTS[] =
{
	// B&W Electrohome G05 / Wells-Gardner V2000 population.
	{ "asteroid", 1.12f },
	{ "astdelux", 1.12f },
	{ "llander",  1.12f },
	{ "omegrace", 1.12f },
	{ "bzone",    1.12f },
	{ "redbaron", 1.12f },

	// Color Wells-Gardner 6100 analysis population.
	{ "spacduel", 1.22f },
	{ "bwidow",   1.22f },
	{ "gravitar", 1.22f },
	{ "tempest",  1.22f },
	{ "quantum",  1.22f },

	// Color Amplifone analysis population.
	{ "mhavoc",   3.43f },
	{ "starwars", 3.43f },
	{ "esb",      3.43f }
};

float timed_beam_gain_for_game(char const *game)
{
	for (timed_beam_gain_entry const &entry : TIMED_BEAM_GAIN_TESTS)
	{
		if (!std::strcmp(game, entry.game))
			return entry.gain;
	}
	return 1.0f;
}

// #define VECTOR_CRT_LOG_COLOR_RESPONSE

constexpr uint64_t TARGET_FLAGS =
		BGFX_TEXTURE_RT |
		BGFX_SAMPLER_U_CLAMP |
		BGFX_SAMPLER_V_CLAMP |
		BGFX_SAMPLER_MIP_POINT;

constexpr uint32_t SAMPLE_FLAGS =
		BGFX_SAMPLER_U_CLAMP |
		BGFX_SAMPLER_V_CLAMP |
		BGFX_SAMPLER_MIP_POINT;

struct beam_vertex
{
	float x;
	float y;
};

struct beam_instance
{
	float x0;
	float y0;
	float x1;
	float y1;
	float red;
	float green;
	float blue;
	float sigma;
	float start;             // display-list-normalized traversal start
	float ramp_duration;     // display-list-normalized X/Y traversal duration
	float intensity;         // normalized vector-generator Z level
	float beam_on_duration;  // display-list-normalized Z-on exposure duration
};

static_assert(sizeof(beam_instance) == sizeof(float) * 12);

} // anonymous namespace


bgfx_vector_renderer::bgfx_vector_renderer(effect_manager &effects, osd_options const &options)
	: m_decay_effect(effects.get_or_load_effect(options, "vector-crt/decay"))
	, m_beam_effect(effects.get_or_load_effect(options, "vector-crt/beam"))
	, m_downsample_effect(effects.get_or_load_effect(options, "vector-crt/downsample"))
	, m_blur_effect(effects.get_or_load_effect(options, "vector-crt/blur"))
	, m_composite_effect(effects.get_or_load_effect(options, "vector-crt/composite"))
	, m_post_vertices(BGFX_INVALID_HANDLE)
	, m_beam_vertices(BGFX_INVALID_HANDLE)
	, m_width(0)
	, m_height(0)
	, m_bloom_width(0)
	, m_bloom_height(0)
	, m_current_accumulation(0)
	, m_last_emu_time(0.0)
	, m_have_time(false)
	, m_reset_accumulation(true)
	, m_available(false)
	, m_present(false)
	, m_persistence(0.0f)
	, m_beam_width(0.0f)
	, m_beam_intensity(0.0f)
	, m_timed_beam_current_gain(timed_beam_gain_for_game(options.system_name()))
	, m_halo(0.0f)
	, m_bloom_strength(0.0f)
	, m_bloom_radius(0.0f)
	, m_exposure(0.0f)
{

	if (!(m_decay_effect && m_beam_effect && m_downsample_effect && m_blur_effect && m_composite_effect))
	{
		osd_printf_verbose("BGFX: Vector CRT renderer: failed to create effects\n");
		return;
	}

	if (!create_geometry())
	{
		osd_printf_verbose("BGFX: Vector CRT renderer: failed to create geometry\n");
		return;
	}
	create_sliders();
	m_available = true;
	osd_printf_verbose("BGFX: Vector CRT renderer initialized\n");
}


bgfx_vector_renderer::~bgfx_vector_renderer()
{
	destroy_targets();
	destroy_geometry();
}


bool bgfx_vector_renderer::create_geometry()
{
	ScreenVertex post[6];
	float const vtop = ((bgfx::getRendererType() == bgfx::RendererType::OpenGL) || (bgfx::getRendererType() == bgfx::RendererType::OpenGLES)) ? 1.0f : 0.0f;
	float const vbottom = 1.0f - vtop;
	auto set_post_vertex = [] (ScreenVertex &vertex, float x, float y, float u, float v)
	{
		vertex.m_x = x;
		vertex.m_y = y;
		vertex.m_z = 0.0f;
		vertex.m_rgba = 0xffffffffU;
		vertex.m_u = u;
		vertex.m_v = v;
	};
	set_post_vertex(post[0], 0.0f, 0.0f, 0.0f, vtop);
	set_post_vertex(post[1], 1.0f, 0.0f, 1.0f, vtop);
	set_post_vertex(post[2], 1.0f, 1.0f, 1.0f, vbottom);
	set_post_vertex(post[3], 1.0f, 1.0f, 1.0f, vbottom);
	set_post_vertex(post[4], 0.0f, 1.0f, 0.0f, vbottom);
	set_post_vertex(post[5], 0.0f, 0.0f, 0.0f, vtop);
	m_post_vertices = bgfx::createVertexBuffer(bgfx::copy(post, sizeof(post)), ScreenVertex::ms_decl);

	beam_vertex const beam[6] =
	{
		{ 0.0f, -1.0f },
		{ 1.0f, -1.0f },
		{ 1.0f,  1.0f },
		{ 1.0f,  1.0f },
		{ 0.0f,  1.0f },
		{ 0.0f, -1.0f }
	};
	m_beam_layout.begin()
		.add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
		.end();
	m_beam_vertices = bgfx::createVertexBuffer(bgfx::copy(beam, sizeof(beam)), m_beam_layout);

	if (!bgfx::isValid(m_post_vertices) || !bgfx::isValid(m_beam_vertices))
	{
		osd_printf_warning("BGFX: Unable to create vector CRT geometry; using normal vector rendering\n");
		destroy_geometry();
		return false;
	}
	return true;
}


void bgfx_vector_renderer::destroy_geometry()
{
	if (bgfx::isValid(m_beam_vertices))
	{
		bgfx::destroy(m_beam_vertices);
		m_beam_vertices = BGFX_INVALID_HANDLE;
	}
	if (bgfx::isValid(m_post_vertices))
	{
		bgfx::destroy(m_post_vertices);
		m_post_vertices = BGFX_INVALID_HANDLE;
	}
}


bool bgfx_vector_renderer::create_targets(uint16_t width, uint16_t height)
{
	destroy_targets();

	if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA16F, TARGET_FLAGS))
	{
		osd_printf_warning("BGFX: RGBA16F render targets are unavailable; using normal vector rendering\n");
		m_available = false;
		return false;
	}

	m_width = width;
	m_height = height;
	m_bloom_width = std::max<uint16_t>(1, (width + 1) / 2);
	m_bloom_height = std::max<uint16_t>(1, (height + 1) / 2);

	auto create_target = [] (target &output, uint16_t target_width, uint16_t target_height)
	{
		output.texture = bgfx::createTexture2D(target_width, target_height, false, 1, bgfx::TextureFormat::RGBA16F, TARGET_FLAGS);
		if (bgfx::isValid(output.texture))
			output.framebuffer = bgfx::createFrameBuffer(1, &output.texture, false);
		return bgfx::isValid(output.texture) && bgfx::isValid(output.framebuffer);
	};

	bool valid = true;
	for (target &accumulation : m_accumulation)
		valid = create_target(accumulation, m_width, m_height) && valid;
	for (target &bloom : m_bloom)
		valid = create_target(bloom, m_bloom_width, m_bloom_height) && valid;

	if (!valid)
	{
		osd_printf_warning("BGFX: Unable to create vector CRT render targets; using normal vector rendering\n");
		destroy_targets();
		m_available = false;
		return false;
	}

	m_current_accumulation = 0;
	m_reset_accumulation = true;
	m_have_time = false;
	osd_printf_verbose("BGFX: Created %ux%u RGBA16F vector phosphor buffers and %ux%u bloom buffers\n", m_width, m_height, m_bloom_width, m_bloom_height);
	return true;
}


void bgfx_vector_renderer::destroy_targets()
{
	auto destroy_target = [] (target &value)
	{
		if (bgfx::isValid(value.framebuffer))
		{
			bgfx::destroy(value.framebuffer);
			value.framebuffer = BGFX_INVALID_HANDLE;
		}
		if (bgfx::isValid(value.texture))
		{
			bgfx::destroy(value.texture);
			value.texture = BGFX_INVALID_HANDLE;
		}
	};
	for (target &accumulation : m_accumulation)
		destroy_target(accumulation);
	for (target &bloom : m_bloom)
		destroy_target(bloom);
	m_width = m_height = m_bloom_width = m_bloom_height = 0;
}


void bgfx_vector_renderer::setup_view(uint16_t view, bgfx::FrameBufferHandle framebuffer, uint16_t width, uint16_t height, bool clear)
{
	bgfx::setViewFrameBuffer(view, framebuffer);
	bgfx::setViewRect(view, 0, 0, width, height);
	bgfx::setViewClear(view, clear ? BGFX_CLEAR_COLOR : BGFX_CLEAR_NONE, 0x00000000U, 1.0f, 0);
	bgfx::setViewMode(view, bgfx::ViewMode::Sequential);

	float projection[16];
	bx::mtxOrtho(projection, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewTransform(view, nullptr, projection);
}


void bgfx_vector_renderer::bind_post_geometry()
{
	bgfx::setVertexBuffer(0, m_post_vertices);
}


void bgfx_vector_renderer::set_uniform(bgfx_effect *effect, char const *name, float x, float y, float z, float w)
{
	if (bgfx_uniform *const uniform = effect->uniform(name))
	{
		float values[4] = { x, y, z, w };
		uniform->set(values, sizeof(values));
	}
}


void bgfx_vector_renderer::draw_post(bgfx_effect *effect, uint16_t view)
{
	bind_post_geometry();
	effect->submit(view);
}


void bgfx_vector_renderer::draw_beams(uint16_t view, double frame_time)
{
	if (m_vectors.empty())
		return;

	bool const have_timing = std::all_of(
			m_vectors.begin(),
			m_vectors.end(),
			[] (render_primitive const *primitive)
			{
				return
						(primitive->vector_start_time >= 0.0F) &&
						(primitive->vector_ramp_duration >= 0.0F) &&
						(primitive->vector_beam_on_duration >= 0.0F) &&
						(primitive->vector_total_duration > 0.0F);
			});

	double const total_duration =
			have_timing
					? m_vectors.front()->vector_total_duration
					: 0.0;
	double const scan_duration =
			have_timing
					? total_duration
					: frame_time;

	// Timed excitation is calibrated by BEAM_ENERGY_RATE_1080. Apply the
	// temporary game/monitor gain experiment only to timed rendering; retain
	// the former user gain for untimed compatibility rendering.
	float const beam_energy_gain =
			have_timing
					? m_timed_beam_current_gain
					: m_beam_intensity;

	float const timed_core_fwhm =
			CORE_FWHM_1080 *
			(float(m_height) / 1080.0f) *
			m_beam_width;
	float const timed_core_sigma =
			timed_core_fwhm /
			GAUSSIAN_FWHM_TO_SIGMA;

	double total_length = 0.0;

#ifdef VECTOR_CRT_LOG_ENERGY_RATE
	double integrated_response = 0.0;
	double timed_beam_energy = 0.0;
#endif

	for (render_primitive const *const primitive : m_vectors)
	{
		double const dx =
				primitive->bounds.x1 -
				primitive->bounds.x0;

		double const dy =
				primitive->bounds.y1 -
				primitive->bounds.y0;

		double const beam_length =
				std::sqrt((dx * dx) + (dy * dy));

		total_length +=
				std::max(
						beam_length,
						std::max<double>(
								primitive->width,
								1.0));

#ifdef VECTOR_CRT_LOG_ENERGY_RATE
		double const sigma =
				std::max(
						have_timing
								? double(timed_core_sigma)
								: double(primitive->width) *
										m_beam_width *
										LEGACY_UNTIMED_BEAM_WIDTH_SCALE *
										LEGACY_UNTIMED_BEAM_SIGMA_SCALE,
						0.01);

		double const intensity =
				std::max<double>(
						primitive->color.a,
						0.0);

		double const core_sigma = sigma;
		double const halo_sigma = sigma * 3.5;
		double const halo_strength = m_halo;

		double const spatial_integral =
				SQRT_TWO_PI *
						((core_sigma +
						  (halo_strength * halo_sigma)) *
						 beam_length) +
				TWO_PI *
						((core_sigma * core_sigma) +
						 (halo_strength *
						  halo_sigma *
						  halo_sigma));

		integrated_response +=
				intensity *
				spatial_integral;

		if (have_timing)
		{
			timed_beam_energy +=
					intensity *
					primitive->vector_beam_on_duration;
		}
#endif
	}

	total_length =
			std::max(
					total_length,
					std::numeric_limits<double>::epsilon());

	/*
	 * Convert the physical beam-time response to the pixel-space
	 * Gaussian integral used by the shader.  The integral scales
	 * with pixel area, hence the square of vertical resolution.
	 */
	double const resolution_scale =
			double(m_height) / 1080.0;

	double const energy_rate =
			BEAM_ENERGY_RATE_1080 *
			resolution_scale *
			resolution_scale;

#ifdef VECTOR_CRT_LOG_COLOR_RESPONSE
	if (have_timing)
	{
		struct color_response_samples
		{
			std::vector<double> intensity;
			std::vector<double> excitation;
			std::vector<double> aged_excitation;
			std::vector<double> excitation_per_intensity;
			std::vector<double> beam_on_per_pixel_1080;
			std::vector<double> predicted_rgb;
		};
		static std::map<int, color_response_samples> samples;
		static double elapsed = 0.0;
		static bool logged = false;
		constexpr double WARMUP_SECONDS = 5.0;
		constexpr double SAMPLE_SECONDS = 60.0;

		if (!logged)
		{
			auto const capsule_integral = [] (double sigma, double length)
			{
				return SQRT_TWO_PI * sigma * length + TWO_PI * sigma * sigma;
			};

			elapsed += frame_time;
			bool const collecting =
					(elapsed > WARMUP_SECONDS) &&
					(elapsed <= (WARMUP_SECONDS + SAMPLE_SECONDS));

			if (collecting)
			for (render_primitive const *const primitive : m_vectors)
			{
				double const intensity = double(primitive->color.a);
				double const beam_on_duration =
						double(primitive->vector_beam_on_duration);
				double const dx = primitive->bounds.x1 - primitive->bounds.x0;
				double const dy = primitive->bounds.y1 - primitive->bounds.y0;
				double const length = std::sqrt(dx * dx + dy * dy);
				if ((intensity <= 0.0) || (beam_on_duration <= 0.0))
					continue;

				double const core_sigma = std::max(double(timed_core_sigma), 0.01);
				double const halo_sigma = core_sigma * 3.5;
				double const filtered_core_sigma =
						std::sqrt(core_sigma * core_sigma + (1.0 / 12.0));
				double const filtered_halo_sigma =
						std::sqrt(halo_sigma * halo_sigma + (1.0 / 12.0));
				double const core_integral = capsule_integral(core_sigma, length);
				double const halo_integral = capsule_integral(halo_sigma, length);
				double const spatial_integral =
						core_integral + double(m_halo) * halo_integral;
				double const peak_radial =
						core_integral / capsule_integral(filtered_core_sigma, length) +
						double(m_halo) * halo_integral /
								capsule_integral(filtered_halo_sigma, length);
				double const excitation =
						intensity * beam_on_duration *
						peak_radial / spatial_integral * energy_rate *
						double(beam_energy_gain);

				double const arrival = std::clamp(
						(double(primitive->vector_start_time) +
						 0.5 * double(primitive->vector_ramp_duration)) /
								total_duration,
						0.0,
						1.0);
				double const scan_persistence = std::max(
						double(m_persistence),
						frame_time * 10.0);
				double const temporal = std::exp(
						-total_duration * (1.0 - arrival) /
						scan_persistence);
				double const aged_excitation = excitation * temporal;

				double const red = double(primitive->color.r);
				double const green = double(primitive->color.g);
				double const blue = double(primitive->color.b);
				int const color_mask =
						((red > 0.5) ? 1 : 0) |
						((green > 0.5) ? 2 : 0) |
						((blue > 0.5) ? 4 : 0);
				if (!color_mask)
					continue;
				bool const is_dot = length <= 0.0001;
				double const excitation_luminance =
						aged_excitation *
						(0.2126 * red + 0.7152 * green + 0.0722 * blue);
				double const emission_scale =
						excitation_luminance > 1.0e-6
								? -std::expm1(-excitation_luminance) /
										excitation_luminance
								: 1.0;
				double const exposure =
						double(TIMED_REFERENCE_EXPOSURE * m_exposure);
				double const hdr_luminance =
						excitation_luminance * emission_scale * exposure;
				double const mapped_scale =
						hdr_luminance > 1.0e-6
								? -std::expm1(-hdr_luminance) / hdr_luminance
								: 1.0;
				double const peak_channel =
						std::max({ red, green, blue }) *
						aged_excitation * emission_scale * exposure * mapped_scale;
				double const predicted_rgb =
						255.0 * std::pow(std::max(peak_channel, 0.0), 1.0 / 2.2);

				auto &color_samples = samples[color_mask | (is_dot ? 8 : 0)];
				color_samples.intensity.emplace_back(intensity);
				color_samples.excitation.emplace_back(excitation);
				color_samples.aged_excitation.emplace_back(aged_excitation);
				color_samples.excitation_per_intensity.emplace_back(
						excitation / intensity);
				if (!is_dot)
				{
					color_samples.beam_on_per_pixel_1080.emplace_back(
							beam_on_duration * resolution_scale / length);
				}
				color_samples.predicted_rgb.emplace_back(predicted_rgb);
			}

			if (elapsed >= (WARMUP_SECONDS + SAMPLE_SECONDS))
			{
				auto const percentile = [] (std::vector<double> &values, double fraction)
				{
					std::sort(values.begin(), values.end());
					return values[size_t(fraction * double(values.size() - 1))];
				};
				for (auto &[sample_key, values] : samples)
				{
					bool const is_dot = bool(sample_key & 8);
					int const color_mask = sample_key & 7;
					osd_printf_verbose(
							"Vector CRT %s color=%d samples=%llu "
							"Z[p25=%g p50=%g p75=%g] "
							"E[p25=%g p50=%g p75=%g] "
							"agedE[p25=%g p50=%g p75=%g] "
							"E/Z[p25=%g p50=%g p75=%g] "
							"RGB[p25=%g p50=%g p75=%g]\n",
							is_dot ? "dot" : "line",
							color_mask,
							(unsigned long long)values.excitation.size(),
							percentile(values.intensity, 0.25),
							percentile(values.intensity, 0.50),
							percentile(values.intensity, 0.75),
							percentile(values.excitation, 0.25),
							percentile(values.excitation, 0.50),
							percentile(values.excitation, 0.75),
							percentile(values.aged_excitation, 0.25),
							percentile(values.aged_excitation, 0.50),
							percentile(values.aged_excitation, 0.75),
							percentile(values.excitation_per_intensity, 0.25),
							percentile(values.excitation_per_intensity, 0.50),
							percentile(values.excitation_per_intensity, 0.75),
							percentile(values.predicted_rgb, 0.25),
							percentile(values.predicted_rgb, 0.50),
							percentile(values.predicted_rgb, 0.75));
					if (!is_dot)
					{
						osd_printf_verbose(
								"  beam-on @1080 [us/pixel]: p25=%g p50=%g p75=%g\n",
								percentile(values.beam_on_per_pixel_1080, 0.25) * 1.0e6,
								percentile(values.beam_on_per_pixel_1080, 0.50) * 1.0e6,
								percentile(values.beam_on_per_pixel_1080, 0.75) * 1.0e6);
					}
				}
				logged = true;
			}
		}
	}
#endif

#ifdef VECTOR_CRT_LOG_ENERGY_RATE
	bool const calibration_valid =
			have_timing &&
			(timed_beam_energy >
					std::numeric_limits<double>::epsilon()) &&
			(integrated_response >
					std::numeric_limits<double>::epsilon());

	if (calibration_valid)
	{
		double const measured_energy_rate =
				integrated_response /
				timed_beam_energy;

		static unsigned log_counter = 0;

		if (!(log_counter++ % 60))
		{
			osd_printf_verbose(
					"Vector CRT: "
					"measured_rate=%g fixed_rate=%g "
					"ratio=%g duration=%g "
					"spatial=%g timed_energy=%g "
					"target=%ux%u\n",
					measured_energy_rate,
					energy_rate,
					measured_energy_rate / energy_rate,
					total_duration,
					integrated_response,
					timed_beam_energy,
					m_width,
					m_height);
		}
	}
#endif

	set_uniform(
			m_beam_effect,
			"u_vector_params",
			float(frame_time),
			m_persistence,
			beam_energy_gain,
			m_halo);

	set_uniform(
			m_beam_effect,
			"u_target_dims",
			float(m_width),
			float(m_height),
			1.0f / float(m_width),
			1.0f / float(m_height));

	set_uniform(
			m_beam_effect,
			"u_vector_timing",
			float(scan_duration),
			float(energy_rate),
			have_timing ? 1.0F : 0.0F,
			0.0F);

	uint32_t offset = 0;
	double elapsed_length = 0.0;

	while (offset < m_vectors.size())
	{
		uint32_t const remaining =
				uint32_t(m_vectors.size() - offset);

		uint32_t const count =
				bgfx::getAvailInstanceDataBuffer(
						remaining,
						sizeof(beam_instance));

		if (!count)
		{
			osd_printf_warning(
					"BGFX: Transient instance buffer exhausted "
					"while rendering vectors\n");
			break;
		}

		bgfx::InstanceDataBuffer instances;

		bgfx::allocInstanceDataBuffer(
				&instances,
				count,
				sizeof(beam_instance));

		beam_instance *const data =
				reinterpret_cast<beam_instance *>(
						instances.data);

		for (uint32_t index = 0; index < count; ++index)
		{
			render_primitive const &primitive =
					*m_vectors[offset + index];

			double const dx =
					primitive.bounds.x1 -
					primitive.bounds.x0;

			double const dy =
					primitive.bounds.y1 -
					primitive.bounds.y0;

			double const beam_length =
					std::sqrt((dx * dx) + (dy * dy));

			double const fallback_length =
					std::max(
							beam_length,
							std::max<double>(
									primitive.width,
									1.0));

			beam_instance &instance =
					data[index];

			instance.x0 = primitive.bounds.x0;
			instance.y0 = primitive.bounds.y0;
			instance.x1 = primitive.bounds.x1;
			instance.y1 = primitive.bounds.y1;

			instance.red = primitive.color.r;
			instance.green = primitive.color.g;
			instance.blue = primitive.color.b;

			instance.sigma =
					have_timing
							? timed_core_sigma
							: primitive.width *
									m_beam_width *
									LEGACY_UNTIMED_BEAM_WIDTH_SCALE *
									LEGACY_UNTIMED_BEAM_SIGMA_SCALE;

			instance.start =
					have_timing
							? float(
									double(primitive.vector_start_time) /
									total_duration)
							: float(
									elapsed_length /
									total_length);

			instance.ramp_duration =
					have_timing
							? float(
									double(primitive.vector_ramp_duration) /
									total_duration)
							: float(
									fallback_length /
									total_length);

			instance.intensity =
					primitive.color.a;
#ifdef VECTOR_CRT_LOG_DOTS
			if (PRIMFLAG_GET_VECTOR_DOT(primitive.flags))
			{
				osd_printf_verbose(
						"Vector CRT dot: "
						"duration=%g total=%g intensity=%g "
						"start=%g pos=(%g,%g)\n",
						double(primitive.vector_ramp_duration),
						double(primitive.vector_total_duration),
						double(primitive.color.a),
						double(primitive.vector_start_time),
						double(primitive.bounds.x0),
						double(primitive.bounds.y0));
			}
#endif
			instance.beam_on_duration =
					have_timing
							? float(
									double(primitive.vector_beam_on_duration) /
									total_duration)
							: instance.ramp_duration;

			if (!have_timing)
			{
				elapsed_length +=
						fallback_length;
			}
		}

		bgfx::setVertexBuffer(
				0,
				m_beam_vertices);

		bgfx::setInstanceDataBuffer(
				&instances,
				0,
				count);

		m_beam_effect->submit(view);

		offset += count;
	}
}


void bgfx_vector_renderer::prepare(uint32_t &view, render_primitive *first, uint16_t width, uint16_t height, double emu_time)
{
	m_vectors.clear();
	m_present = false;
	for (render_primitive *primitive = first; primitive; primitive = primitive->next())
	{
		m_present = m_present || bool(PRIMFLAG_GET_VECTORBUF(primitive->flags));
		if ((primitive->type == render_primitive::LINE) && PRIMFLAG_GET_VECTOR(primitive->flags))
			m_vectors.push_back(primitive);
	}

	if (!m_available || !m_present || !width || !height)
		return;

	if ((width != m_width) || (height != m_height))
	{
		if (!create_targets(width, height))
			return;
	}

	double frame_time = 1.0 / 60.0;
	if (m_have_time)
	{
		frame_time = emu_time - m_last_emu_time;

		// Time can move backwards after a reset, state load, rewind, or
		// machine restart. Clear accumulated excitation instead of retaining an image
		// from the previous timeline.
		if (frame_time < 0.0)
		{
			m_reset_accumulation = true;
			frame_time = 1.0 / 60.0;
		}
		else if (frame_time <= 0.000001)
		{
			// Emulation is paused or this display list has already been
			// processed at the same emulated time. Do not excite it again.
			return;
		}

		// Avoid an unusually large elapsed time causing undesirable behaviour.
		frame_time = std::min(frame_time, 0.100);
	}
	m_last_emu_time = emu_time;
	m_have_time = true;

	uint8_t const previous = m_current_accumulation;
	uint8_t const current = previous ^ 1;

	setup_view(
			uint16_t(view),
			m_accumulation[current].framebuffer,
			m_width,
			m_height,
			m_reset_accumulation);

	if (m_reset_accumulation)
	{
		bgfx::touch(uint16_t(view));
		m_reset_accumulation = false;
	}
	else
	{
		// Exponential excitation decay:
		//
		//     E(t + dt) = E(t) * exp(-dt / tau)
		//
		// m_persistence is tau in seconds.
		float const tau = std::max(m_persistence, 0.001f);
		float const decay = std::exp(-float(frame_time) / tau);

		bgfx_uniform *const sampler = m_decay_effect->uniform("s_accum");
		if (!sampler)
		{
			osd_printf_warning("BGFX: Vector CRT decay effect has no s_accum uniform\n");
			m_available = false;
			return;
		}

		bgfx::setTexture(
				0,
				sampler->handle(),
				m_accumulation[previous].texture,
				SAMPLE_FLAGS);

		set_uniform(m_decay_effect, "u_decay", decay);
		draw_post(m_decay_effect, uint16_t(view));
	}
	++view;
	m_current_accumulation = current;

	if (!m_vectors.empty())
	{
		setup_view(uint16_t(view), m_accumulation[current].framebuffer, m_width, m_height, false);
		float projection[16];
		bx::mtxOrtho(projection, 0.0f, float(m_width), float(m_height), 0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
		bgfx::setViewTransform(uint16_t(view), nullptr, projection);
		draw_beams(uint16_t(view), frame_time);
		++view;
	}

	setup_view(uint16_t(view), m_bloom[0].framebuffer, m_bloom_width, m_bloom_height, false);
	bgfx::setTexture(0, m_downsample_effect->uniform("s_accum")->handle(), m_accumulation[current].texture, SAMPLE_FLAGS);
	set_uniform(m_downsample_effect, "u_source_texel", 1.0f / float(m_width), 1.0f / float(m_height));
	draw_post(m_downsample_effect, uint16_t(view));
	++view;

	// Scale the output-pixel bloom radius relative to a 1080-line reference.
	float const bloom_scale = std::clamp(float(m_height) / 1080.0f, 0.25f, 2.0f);
	float const bloom_radius = m_bloom_radius * bloom_scale;
	float const sigma_x = bloom_radius * float(m_bloom_width) / float(m_width);
	float const sigma_y = bloom_radius * float(m_bloom_height) / float(m_height);

	// Gaussian variances add over repeated passes.  Divide sigma so the
	// combined blur retains the radius requested by the slider.
	float const pass_scale = 1.0f / std::sqrt(float(BLOOM_PASSES));
	float const pass_sigma_x = sigma_x * pass_scale;
	float const pass_sigma_y = sigma_y * pass_scale;

	for (unsigned pass = 0; pass < BLOOM_PASSES; ++pass)
	{
		setup_view(uint16_t(view), m_bloom[1].framebuffer, m_bloom_width, m_bloom_height, false);
		bgfx::setTexture(0, m_blur_effect->uniform("s_tex")->handle(), m_bloom[0].texture, SAMPLE_FLAGS);
		set_uniform(m_blur_effect, "u_blur", 1.0f / float(m_bloom_width), 0.0f, pass_sigma_x);
		draw_post(m_blur_effect, uint16_t(view));
		++view;

		setup_view(uint16_t(view), m_bloom[0].framebuffer, m_bloom_width, m_bloom_height, false);
		bgfx::setTexture(0, m_blur_effect->uniform("s_tex")->handle(), m_bloom[1].texture, SAMPLE_FLAGS);
		set_uniform(m_blur_effect, "u_blur", 0.0f, 1.0f / float(m_bloom_height), pass_sigma_y);
		draw_post(m_blur_effect, uint16_t(view));
		++view;
	}
}


void bgfx_vector_renderer::composite(uint16_t view)
{
	if (!m_available || !m_present || !m_width || !m_height)
		return;

	bgfx::setTexture(0, m_composite_effect->uniform("s_accum")->handle(), m_accumulation[m_current_accumulation].texture, SAMPLE_FLAGS);
	bgfx::setTexture(1, m_composite_effect->uniform("s_bloom")->handle(), m_bloom[0].texture, SAMPLE_FLAGS);
	bool const have_timing =
			!m_vectors.empty() &&
			std::all_of(
					m_vectors.begin(),
					m_vectors.end(),
					[] (render_primitive const *primitive)
					{
						return
								(primitive->vector_start_time >= 0.0F) &&
								(primitive->vector_ramp_duration >= 0.0F) &&
								(primitive->vector_beam_on_duration >= 0.0F) &&
								(primitive->vector_total_duration > 0.0F);
					});
	float const base_exposure =
			have_timing
					? TIMED_REFERENCE_EXPOSURE
					: LEGACY_UNTIMED_EXPOSURE;
	set_uniform(
			m_composite_effect,
			"u_composite",
			m_bloom_strength,
			base_exposure * m_exposure,
			2.2f,
			0.0f);
	draw_post(m_composite_effect, view);
}


void bgfx_vector_renderer::create_sliders()
{
	struct slider_description
	{
		char const *name;
		int32_t minimum;
		int32_t value;
		int32_t maximum;
		int32_t increment;
	};
	static constexpr slider_description descriptions[SLIDER_COUNT] =
	{
		{ "Vector phosphor persistence",   1,   2, 100, 1 },   // 0.02
		{ "Vector core FWHM scale",       30, 100, 400, 1 },   // 1.00
		{ "Vector untimed beam intensity", 10, 400, 500, 1 },  // 4.00
		{ "Vector beam halo",              0,   4, 100, 1 },   // 0.04
		{ "Vector bloom strength",         0,  20, 300, 1 },   // 0.20
		{ "Vector bloom radius",          50, 212, 260, 1 },   // 2.12
		{ "Vector exposure scale",        10, 100, 400, 1 },   // 1.00
	};

	m_sliders.reserve(SLIDER_COUNT);
	for (uint8_t id = 0; id < SLIDER_COUNT; ++id)
	{
		slider_description const &description = descriptions[id];
		m_sliders.emplace_back(std::make_unique<slider_state>(
				description.name,
				description.minimum,
				description.value,
				description.maximum,
				description.increment,
				[this, id] (std::string *text, int32_t value) { return slider_changed(slider_id(id), text, value); }));
		slider_changed(slider_id(id), nullptr, description.value);
	}
}


int32_t bgfx_vector_renderer::slider_changed(slider_id id, std::string *text, int32_t new_value)
{
	float scale = 1.0f;
	float *setting = nullptr;
	char const *format = "%1.2f";
	switch (id)
	{
		case SLIDER_PERSISTENCE:
			setting = &m_persistence;
			scale = 0.001f;
			format = "%1.0f ms";
			break;
		case SLIDER_BEAM_WIDTH:
			setting = &m_beam_width;
			scale = 0.01f;
			format = "%1.2fx";
			break;
		case SLIDER_BEAM_INTENSITY:
			setting = &m_beam_intensity;
			scale = 0.01f;
			format = "%1.2fx";
			break;
		case SLIDER_HALO:
			setting = &m_halo;
			scale = 0.01f;
			format = "%1.2fx";
			break;
		case SLIDER_BLOOM_STRENGTH:
			setting = &m_bloom_strength;
			scale = 0.01f;
			format = "%1.2fx";
			break;
		case SLIDER_BLOOM_RADIUS:
			setting = &m_bloom_radius;
			scale = 0.01f;
			format = "%1.2f px @ 1080p";
			break;
		case SLIDER_EXPOSURE:
			setting = &m_exposure;
			scale = 0.01f;
			format = "%1.2fx";
			break;
		default:
			return 0;
	}

	if (new_value != SLIDER_NOCHANGE)
		*setting = float(new_value) * scale;
	if (text)
	{
		float const display_value = (id == SLIDER_PERSISTENCE) ? (*setting * 1000.0f) : *setting;
		*text = util::string_format(format, display_value);
	}
	return int32_t(std::floor((*setting / scale) + 0.5f));
}


void bgfx_vector_renderer::append_sliders(std::vector<ui::menu_item> &items)
{
	if (!m_available || !m_present)
		return;
	for (std::unique_ptr<slider_state> const &slider : m_sliders)
	{
		ui::menu_item item(ui::menu_item_type::SLIDER, slider.get());
		item.set_text(slider->description);
		items.emplace_back(std::move(item));
	}
}
