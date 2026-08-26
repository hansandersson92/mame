BGFX vector CRT renderer
========================

.. contents:: :local:


Purpose and status
------------------

The BGFX vector CRT renderer is a GPU pipeline for MAME vector games.  It
replaces direct, frame-oriented line drawing with a persistent HDR phosphor
excitation buffer, instanced beam-energy deposition, phosphor emission,
optical bloom, and display composition.  AVG/DVG supplies hardware-derived
display-list timestamps, X/Y ramp duration, Z-on duration, and total list
duration.  Generators without this information use a separate compatibility
path with length-derived timing and the former intensity-shaped beam profile.

The ``bgfx_vectorcrt`` option is enabled by default; set it to ``0`` to disable
it.  The renderer is used only with the BGFX video backend and when the
primitive list contains a vector-buffer marker.  If its effects, geometry, or
RGBA16F render targets cannot be created, BGFX falls back to normal vector line
rendering.


Source map
----------

The implementation is split across these locations:

* ``src/osd/modules/render/bgfx/vectorrenderer.cpp`` and ``.h`` manage render
  targets, instance generation, pass scheduling, emulation time, and sliders.
* ``src/emu/vector.cpp`` and ``render.cpp`` carry vector-generator timing and
  unclamped beam drive through render primitives.
* ``src/devices/video/avgdvg.cpp`` supplies authoritative AVG/DVG timing and
  board-specific Z intensity.
* ``src/osd/modules/render/drawbgfx.cpp`` and ``.h`` integrate the renderer,
  intercept vector primitives, preserve draw ordering, and composite output.
* ``src/osd/modules/render/bgfx/shaders/chains/vector-crt/`` contains shader
  source and varying definitions.
* ``bgfx/effects/vector-crt/`` contains render state, uniforms, samplers, and
  shader program selection.
* ``bgfx/shaders/<backend>/chains/vector-crt/`` contains compiled shaders for
  Direct3D 9, Direct3D 11, OpenGL ES, OpenGL, Metal, and SPIR-V.


Pipeline overview
-----------------

.. code-block:: text

    Ordered MAME vector primitives
                |
                v
    CPU instance buffer (one record per segment)
                |
                v
    Previous RGBA16F accumulation -- exponential decay --+
                                                           |
    Instanced core-plus-tail beam deposition --------------+
                |
                +----> persistent full-resolution excitation
                |
                v
    Phosphor emission + half-resolution box downsample
                |
                v
    Two horizontal/vertical Gaussian blur iterations
                |
                v
    Excitation -> phosphor emission -> monitor colour transform
               -> bloom -> exposure -> display shoulder -> gamma -> output

The persistent accumulation is ping-ponged between two full-resolution
RGBA16F targets.  Bloom uses two RGBA16F targets rounded up to half the output
dimensions:

.. code-block:: cpp

    bloom_width  = max(1, (width  + 1) / 2);
    bloom_height = max(1, (height + 1) / 2);

RGBA16F is required because additive beam contributions must retain HDR energy
across frames before phosphor emission and display mapping.


Integration and frame ordering
------------------------------

``renderer_bgfx`` creates ``bgfx_vector_renderer`` when ``bgfx_vectorcrt`` is
enabled.  At the start of a frame, ``prepare()`` scans the primitive list:

* ``PRIMFLAG_VECTORBUF`` identifies a vector screen and activates the renderer.
* Line primitives with ``PRIMFLAG_VECTOR`` are collected in display-list order.

These vector lines are omitted from normal BGFX line batching.  The completed
CRT image is composited where the vector-buffer marker occurs, preserving its
order relative to artwork and other primitives.

Each active frame performs the following operations:

#. Decay or clear the persistent phosphor target.
#. Add all vector beam instances to the same target.
#. Convert excitation to emitted light and downsample it into the first bloom
   target.
#. Apply two iterations of horizontal and vertical bloom blur.
#. Convert persistent excitation to direct emitted light and composite it with
   bloom into the configured BGFX view.

Views use sequential mode because later passes consume textures written by
earlier passes.


Time and phosphor persistence
-----------------------------

The renderer uses emulated time rather than wall-clock time:

* The first frame assumes 1/60 second.
* A repeated or effectively zero timestamp is ignored, preventing repeated
  excitation of the same display list while paused.
* Backward time after reset, state load, or rewind clears the accumulation.
* A forward interval is capped at 0.100 second to avoid extreme pass values.

For frame interval ``dt`` and persistence time constant ``tau``, the decay pass
applies:

.. code-block:: text

    decay = exp(-dt / max(tau, 0.001))
    E_next = E_previous * decay

This is inter-frame phosphor decay.  Scan-order attenuation within newly drawn
geometry uses a deliberately gentler lifetime described below.


CPU instance generation
-----------------------

Every vector segment becomes one 12-float instance:

.. code-block:: cpp

    struct beam_instance
    {
        float x0, y0, x1, y1;       // target-pixel endpoints
        float red, green, blue;      // beam colour
        float sigma;                 // core Gaussian sigma in target pixels
        float start;                 // normalised traversal start
        float ramp_duration;         // normalised X/Y traversal duration
        float intensity;             // vector-generator beam drive
        float beam_on_duration;      // normalised Z-on duration
    };

The instance vectors seen by the vertex shader are:

.. code-block:: text

    i_data0 = (x0, y0, x1, y1)
    i_data1 = (red, green, blue, sigma)
    i_data2 = (start, ramp_duration, intensity, beam_on_duration)


Timed and compatibility scan timing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

AVG/DVG records each operation on its authoritative state-machine clock.  A
rendered point carries:

.. code-block:: text

    start_time        = X/Y traversal start relative to the list epoch
    ramp_duration     = X/Y deflection/traversal duration
    beam_on_duration  = effective Z-on exposure duration
    total_duration    = furthest scheduled list endpoint relative to the epoch

``beam_on_duration`` is not added to ``ramp_duration`` because the intervals
can overlap.  Gaps, blank moves, clipped vectors, and rejected vectors need no
dummy geometry: their hardware time is already reflected in later absolute
start times and in ``total_duration``.  A stable list epoch lets Major Havoc
append batches without restarting their timestamps near zero.

When every visible primitive contains valid timing, the CPU normalises the
three per-vector values by ``total_duration``.  Untimed generators instead
approximate constant deflection speed using accumulated visible length:

.. code-block:: text

    length = max(endpoint_distance, max(primitive_width, 1 pixel))
    start = elapsed_length / total_length
    duration = length / total_length

The width/one-pixel lower bound gives points and degenerate segments a non-zero
fallback interval.  This compatibility approximation preserves display-list
order but does not account for blanked moves or actual generator timing.


Timed core width and resolution scaling
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The timed path does not use ``render_primitive::width``.  That width contains
MAME's legacy intensity-dependent widening, while timed intensity already
represents relative beam current and deposited energy.  The physical core is
instead calibrated explicitly as FWHM at a reference vertical resolution:

.. code-block:: cpp

    constexpr float CORE_FWHM_1080 = 1.0f;
    core_fwhm = CORE_FWHM_1080 * output_height / 1080.0f;
    sigma = core_fwhm / 2.354820045f;

One output pixel at 1080 lines is a provisional rendering calibration, not a
measurement of a particular CRT.  At lower resolutions the physical core may
be subpixel; pixel-footprint integration preserves its energy rather than
silently clamping its width to one pixel.

The untimed compatibility path retains the previous primitive-width mapping:

.. code-block:: cpp

    sigma = primitive.width * 0.75f * 0.085f;

Bloom differs because its radius originates as a UI pixel value referenced to
1080 lines, so it requires an explicit resolution scale.


Beam shaders
------------


Vertex stage
~~~~~~~~~~~~

The shared mesh is a six-vertex unit quad.  ``vs_beam.sc`` expands it around
each segment in target-pixel space:

#. Compute segment direction, perpendicular normal, and length.
#. Evaluate the same core and broad-profile widths used by the fragment stage.
#. Add the approximate pixel-box variance to both widths.
#. Extend both endpoints by six times the widest filtered sigma, with a
   one-pixel minimum.
#. Emit the padded quad through a target-pixel orthographic projection.
#. Pass beam-local ``along`` and ``across`` coordinates to the fragment shader.

Sizing geometry from the widest evaluated component prevents the broad tail or
legacy halo from being clipped into a rectangular hot spot.  Longitudinal
padding provides pixels for round endpoint caps.  Zero-length segments use
``(1, 0)`` as a stable fallback direction.


Timed core-plus-tail profile
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Timed beam drive is transported independently of the compatibility alpha
range and is not clamped to one.  Higher drive grows both spatial components
and transfers a bounded fraction of deposited energy into the broad Gaussian
tail:

.. code-block:: text

    widthResponse = max(drive, 0)
    coreSigma = baseSigma * (1 + 0.1 * widthResponse)
    tailSigma = 2.5 * baseSigma * (1 + 0.5 * widthResponse)
    tailFraction = maximumTailFraction * drive^2 / (drive^2 + 1)

The default maximum tail fraction is 0.60.  This is an asymptotic limit: at
drive 1.0 the actual fraction is 0.30.  Core and tail are independently
normalised before being mixed, preserving deposited energy for lines and
stationary dots.  This electronic/phosphor spot tail is an empirical
approximation of a non-Gaussian high-current spot and is distinct from later
optical bloom.  The width response deliberately has no upper clamp, allowing
board-level drive above 1.0 to produce additional spot growth.

Untimed generators retain the older compatibility profile:

.. code-block:: text

    response = sqrt(clamp(intensity, 0, 1))
    coreSigma = baseSigma * (1 + 0.12 * response)
    haloSigma = baseSigma * 3.5 * (1 + 0.25 * response)
    haloStrength = haloControl * mix(0.6, 1.0, response)

Outside the segment endpoints, longitudinal and perpendicular distance are
combined for round caps:

.. code-block:: text

    pastEndpoint = max(-along, along - beamLength, 0)
    distanceSquared = across^2 + pastEndpoint^2


Pixel-footprint filtering
~~~~~~~~~~~~~~~~~~~~~~~~~

Very narrow analytic Gaussians alias if sampled only at pixel centres.  The
fragment shader approximates a pixel as a box filter.  For footprint
``pixelAcross``, box variance is ``pixelAcross^2 / 12`` and is added to beam
variance:

.. code-block:: text

    filteredSigma = sqrt(sigma^2 + pixelVariance)

For a Gaussian distance field around a finite segment, the complete spatial
integral is:

.. code-block:: text

    capsuleIntegral(sigma, length)
        = sqrt(2*pi) * sigma * length + 2*pi * sigma^2

The first term is the swept-line body and the second is the two endpoint caps.
Each filtered component is multiplied by the ratio of its unfiltered and
filtered capsule integrals.  This preserves energy continuously from a
stationary two-dimensional spot through short segments to long lines, without
an ``isDot`` branch.


Scan-order variation
~~~~~~~~~~~~~~~~~~~~

For a fragment on the physical segment:

.. code-block:: text

    segmentPosition = clamp(along / beamLength, 0, 1)
    arrival = clamp(start + duration * segmentPosition, 0, 1)
    age = displayListDuration * (1 - arrival)

A degenerate segment uses position 0.5.  Earlier vectors have greater age at
presentation and are attenuated slightly more:

.. code-block:: text

    scanPersistence = max(phosphorPersistence, frameInterval * 10)
    temporal = exp(-age / max(scanPersistence, 0.001))

The ten-frame floor prevents short physical persistence from nearly erasing
vectors at the beginning of the display list before presentation.  It keeps
scan variation subtle; inter-frame decay still uses physical persistence.

For timed vectors, Z-on exposure and the resolution-scaled energy calibration
produce:

.. code-block:: text

    depositedEnergy = drive * beamOnDuration * energyRate
    energy = colour * depositedEnergy * normalisedSpatialProfile * temporal

``BEAM_ENERGY_RATE_1080`` maps a measured Asteroids reference core response of
``2.72562e-7`` to 0.10 pre-emission excitation.  This establishes a useful
phosphor operating point; it is not an absolute luminance measurement.  The
rate scales with the square of output height because the pixel-space spatial
integral scales with pixel area.

Untimed rendering instead uses the compatibility intensity and halo controls
without duration-based deposited energy.

The beam effect uses additive blending into the current accumulation target.


Bloom
-----

The full-resolution buffer stores excitation, not visible light.  Before
downsampling, each of the four box-filter samples is converted through the
phosphor-emission response.  Bloom therefore follows emitted light after local
phosphor saturation rather than unbounded deposited excitation.

The reduced image uses a separable nine-tap Gaussian kernel.  Each axis samples
the centre texel and integer offsets from one through four texels in both
directions.  The fragment shader calculates weights from the uniform sigma and
normalises them for unit DC gain.  Two complete horizontal/vertical iterations
produce four blur draws per frame.

Bloom radius is referenced to 1080 lines:

.. code-block:: cpp

    bloom_scale = clamp(output_height / 1080.0f, 0.25f, 2.0f);
    bloom_radius = m_bloom_radius * bloom_scale;

This keeps bloom approximately constant as a fraction of screen height through
2160p.  The clamp prevents extreme kernels outside that range.  The radius is
converted from output pixels to bloom-texture texels independently for each
axis:

.. code-block:: cpp

    sigma_x = bloom_radius * bloom_width  / output_width;
    sigma_y = bloom_radius * bloom_height / output_height;

    pass_scale   = 1 / sqrt(bloom_passes);
    pass_sigma_x = sigma_x * pass_scale;
    pass_sigma_y = sigma_y * pass_scale;

    horizontal_u_blur = (1 / bloom_width,  0,                pass_sigma_x, 0);
    vertical_u_blur   = (0,                1 / bloom_height, pass_sigma_y, 0);

``u_blur.xy`` is therefore exactly one bloom texel along the active axis, and
``u_blur.z`` is Gaussian sigma in bloom texels.  The fragment shader samples
at integer multiples of that texel step.  Minification and magnification remain
linearly filtered, but the kernel no longer relies on fractional bilinear tap
positions.  Gaussian variances add across the repeated iterations, so dividing
each pass's sigma by the square root of the pass count makes the combined blur
match the radius requested by the slider.

There is no brightness threshold.  All positive emitted light contributes,
and ``Vector bloom strength`` controls its optical contribution during
composition.  The default is deliberately low because the timed beam profile
already contains a current-dependent broad spot tail.


Composite
---------

The stored excitation first becomes emitted phosphor light using a
luminance-preserving nonlinear response:

.. code-block:: text

    emittedLuminance = 1 - exp(-excitationLuminance)

Monochrome output then receives a provisional blue-white P4 transform.  Once
the renderer observes an emitting chromatic primitive, it retains a generic
P22 colour transform derived from MAME's HLSL chromaticity defaults.  These are
period-appropriate approximations, not measurements of individual tubes.

The composite shader combines direct emitted light and bloom, then applies the
path-specific reference exposure and the user exposure scale:

.. code-block:: text

    hdr = monitorTransform(phosphor + bloom * bloomStrength)
          * referenceExposure * exposureScale

Timed rendering preserves ordinary linear luminance ratios and introduces a
smooth, slope-continuous shoulder only above the 0.75 SDR knee:

.. code-block:: text

    L = dot(hdr, (0.2126, 0.7152, 0.0722))
    mappedL = L                                      when L <= 0.75
    mappedL = 0.75 + 0.25 * (1 - exp(-(L-0.75)/0.25)) otherwise
    mappedRGB = hdr * mappedL / L

This late shoulder avoids the former double compression in which both phosphor
emission and an exponential display mapper compressed ordinary and bright
vectors together.  Untimed compatibility rendering retains its previous
``1-exp(-L)`` display mapping and reference exposure.  Scaling RGB by the
luminance ratio retains hue and saturation.  The result receives a ``1/2.2``
gamma transform.  The shader supports edge vignette through
``u_composite.w``, but C++ currently sets it to zero.


Runtime controls
----------------

Sliders appear only while a vector screen is present.  Construction explicitly
invokes each slider callback, so temporary zero-valued member initialisation
cannot reach rendering.

.. list-table:: Vector CRT controls
   :header-rows: 1
   :widths: 28 18 14 40

   * - Control
     - Range
     - Default
     - Meaning
   * - Vector phosphor persistence
     - 1-100 ms
     - 2 ms
     - Inter-frame exponential time constant
   * - Vector untimed beam intensity
     - 0.10-5.00x
     - 4.00x
     - Compatibility-path beam energy gain
   * - Vector untimed beam halo
     - 0.00-1.00x
     - 0.04x
     - Compatibility-path wide Gaussian strength
   * - Vector beam tail fraction
     - 0.00-1.00
     - 0.60
     - Maximum timed energy fraction transferred to the current-grown broad tail
   * - Vector bloom strength
     - 0.00-0.20x
     - 0.05x
     - Bloom contribution during composition
   * - Vector bloom radius
     - 0.50-2.60 px at 1080p
     - 2.12
     - Blur radius before resolution scaling
   * - Vector exposure scale
     - 0.10-4.00x
     - 1.00x
     - Multiplier on the timed or compatibility reference exposure

Core vector controls still affect primitive colour and the untimed line-width
fallback.  Timed core sigma is independent of ``primitive.width``.  Timed beam
drive is transported separately so board-level values above the compatibility
0-255 intensity range, such as Star Wars analog overdrive, remain available to
the physical energy model.


Performance characteristics
---------------------------

CPU work is linear in segment count: collect primitives, measure lengths, and
fill transient instance buffers.  There is no CPU per-pixel rasterisation.

GPU beam cost is proportional to padded quad area.  Bloom has one half-
resolution downsample and four half-resolution blur passes.  Decay and
composition are full-resolution.  Memory is dominated by two full-resolution
and two half-resolution RGBA16F targets.

Transient instance-buffer exhaustion is handled in batches.  Failure to
allocate any instances logs a warning and stops beam submission for that frame.


Historical 4K performance and VSync
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The following development measurement predates the direct AVG/DVG timing,
fixed-FWHM core, current-dependent tail, and current bloom defaults.  It is
retained as an implementation-cost reference, not a benchmark of the current
calibration.

A Star Wars test at 4K used this development machine:

.. list-table:: Benchmark host
   :header-rows: 1
   :widths: 24 76

   * - Component
     - Specification
   * - System
     - BESSTAR TECH LIMITED DMAF5 mini PC
   * - CPU
     - AMD Ryzen 5 3550H, 4 cores and 8 logical processors; 2.10 GHz reported
       maximum clock
   * - GPU
     - Integrated AMD Radeon Vega 8 Graphics
   * - GPU driver
     - 31.0.12027.9001, dated 2023-03-30
   * - Memory
     - 16 GB installed; approximately 15.7 GiB visible to Windows
   * - Operating system
     - Microsoft Windows 10 Home 64-bit, version/build 10.0.19045
   * - Display mode
     - 3840 by 2160 at 60 Hz

Windows reports a 256 MiB adapter-memory allocation for Vega 8, but the
integrated GPU also uses shared system memory.  This is not total memory
available to the renderer.

.. list-table:: Star Wars 4K benchmark
   :header-rows: 1
   :widths: 70 30

   * - Mode
     - Average speed
   * - Vector CRT with normal ``waitvsync``
     - 94.85%
   * - Vector CRT with ``-nowaitvsync``
     - 100.00%
   * - Vector CRT with ``-nowaitvsync -nothrottle``
     - 204.04%

The standard renderer reached approximately 99.8% with normal ``waitvsync`` on
the same machine.  The vector CRT renderer can sustain real-time 4K rendering
in this test and has roughly twice-real-time raw throughput when synchronisation
and throttling are disabled.  These results are not portable guarantees.

The approximately 5% difference is related to synchronised presentation rather
than inability to render the scene.  The 94.85% result does not indicate GPU
saturation: disabling VSync restores 100%, and disabling throttling exposes
substantial headroom.  The likely cause is an interaction between Star Wars'
native update rate, MAME throttling, presentation timing, and VSync.

Use ``-nowaitvsync -nothrottle`` to compare raw throughput.  Test normal
synchronised operation separately for user-visible frame pacing.  Both matter:
fewer bloom passes may reduce latency, power, and utilisation without being
required for real time, while default-on behaviour must account for users who
observe sub-100% speed with normal ``waitvsync``.


Screenshots
-----------

Asteroids (monochrome vector display)

.. image:: ../images/vectorcrt_asteroids.png
   :width: 700px
   :alt: Asteroids rendered with the BGFX vector CRT renderer

Tempest (colour vector display)

.. image:: ../images/vectorcrt_tempest.png
   :width: 700px
   :alt: Tempest rendered with the BGFX vector CRT renderer


Building and validation
-----------------------

After changing a vector CRT shader, rebuild every supported backend with:

.. code-block:: shell

    make shaders CHAIN=vector-crt -j4

Expected binary changes appear below
``bgfx/shaders/{dx9,dx11,essl,glsl,metal,spirv}/chains/vector-crt/``.

Useful validation systems include Asteroids, Battlezone, Tempest, and Star
Wars.  Check at minimum:

* Thin diagonal, horizontal, and vertical vectors for aliasing and equal energy.
* Equal-drive lines of different length for consistent local excitation.
* Points and short segments for round caps and continuous capsule-integral
  energy preservation.
* Bright text and dots for a sharp core, broad current-dependent tail, and no
  rectangular quad clipping.
* Early and late display-list geometry for scan-order variation using the
  generator-supplied total duration.
* Timed AVG/DVG and untimed generators to verify that their calibration paths
  remain separate.
* Pause, reset, state load, rewind, and large emulation-time steps.
* 1080p and 2160p output for proportional beam width and bloom radius.
* Views with artwork for correct screen scaling and composite order.


Current limitations and future work
-----------------------------------

Potential improvements include:

* Hardware-derived timestamps, blanked-move timing, and dwell time for vector
  generators other than AVG/DVG.
* Independent RGB decay constants and measured phosphor spectra.
* Deflection-amplifier slew limits, endpoint ringing, overshoot, and jitter.
* Position-dependent focus or convergence.
* Measured monitor-specific core width, beam-tail profile, and optical
  point-spread functions.
* HDR-native output and display-aware calibration.
* Multi-monitor vector timing and resource ownership.

These extensions should preserve the architecture: the CPU uploads ordered
metadata while beam rasterisation, persistence, bloom, and display mapping
remain GPU operations.
