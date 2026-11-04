/******************************************************************************
 * requantizer.h
 * rfx library - re-quantising a capture to as few bits as its own noise allows
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools. It is the adaptive requantiser of the
 * Domesday Duplicator's capture application (ddd-gui, after rfquant's dynamic
 * mode), ported without changing what it decides: the same settings and the same
 * samples give the same samples out.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rfx {

// The adaptive re-quantiser.
//
// A capture's low bits carry the converter's quantisation noise and the front
// end's analogue noise, and the analogue noise is usually the larger: dropping a
// bit whose contribution is buried under it costs nothing anybody can measure and
// saves nearly a bit a sample in a FLAC file (and in an .rfx file, whose `shift`
// makes the zero bits free). How many can go depends on how noisy this capture
// is, and that changes with the disc, the player and the moment, so it is decided
// again for every segment of about 35 ms, from the noise floor of the last half
// second.
//
// The decision is made per 1 MHz slice of the protected bands: a setting is
// allowed when the noise it adds raises the floor of the worst slice by no more
// than the margin's limit. The added noise is known analytically (the variance of
// a requantisation, shaped by the noise transfer function where noise shaping is
// used), so trying a candidate costs a sum, not a pass over the samples.
//
// Noise shaping pushes the added noise out of the protected bands, where the
// decoder's filters remove it, so a shaped setting passes at fewer bits than an
// unshaped one. The filter is designed by Levinson-Durbin from the wanted
// weighting: A(z) minimum phase, |A|^2 proportional to 1/weight.
//
// Samples are signed 16-bit and left-aligned, as every writer produces them; a
// re-quantised sample is still one, with more zero bits at the bottom.

// A decimal with `decimals` digits after the point, the same in every locale.
std::string formatDecimal(double value, int decimals);

// A band the decoder needs, in MHz.
struct FrequencyBand
{
    double lowMhz = 0.0;
    double highMhz = 0.0;

    bool operator==(const FrequencyBand &other) const { return lowMhz == other.lowMhz && highMhz == other.highMhz; }
};

// How much the noise floor of any 1 MHz slice of the protected bands may rise,
// and how long a more aggressive setting has to stay justified before it is
// taken. Five levels, from aggressive to ultra safe.
constexpr int kMinimumMarginLevel = 0;
constexpr int kMaximumMarginLevel = 4;
constexpr int kDefaultMarginLevel = 2;

double marginLimitDb(int level);
double marginHoldSeconds(int level);
const char *marginLevelName(int level);

// The most of the input's bits a segment can lose. The quality limit decides in
// practice; this only bounds how many candidates are tried.
constexpr int kMaximumRequantizerDrop = 6;

// From DC to 14 MHz (the LaserDisc's RF), or to 1.5 MHz short of the Nyquist
// limit at a rate that cannot hold all of that.
std::vector<FrequencyBand> defaultProtectedBands(double sampleRateMhz);

// The bands as they are reported: "0-14 MHz", comma separated.
std::string describeBands(const std::vector<FrequencyBand> &bands);

// Bands as a person types them, in MHz: "0-12", or several, "0-1.9,2.1-13.5",
// with ':' accepted for '-' and spaces and a trailing "MHz" ignored. Plain
// decimals, read the same whatever the locale. Each band's low end below its high
// end and none negative; empty for anything else.
std::vector<FrequencyBand> parseBands(const std::string &text);

// `bands` as they can be used at `sampleRateMhz`: cut at the Nyquist limit, and
// any left with nothing in them dropped. Empty when none survive.
std::vector<FrequencyBand> bandsWithin(const std::vector<FrequencyBand> &bands, double sampleRateMhz);

// A stretch outside the protected bands with a shaping depth of its own: how far
// above the bands the added noise may be pushed there, in place of the one depth
// everywhere else. Outside the bands is not all alike (the EFM below the video is
// still read, and the top of the spectrum is not), so a shallow zone over the one
// keeps the noise off it, and a deep one over the other takes what it is spared.
struct ShapingZone
{
    double lowMhz = 0.0;
    double highMhz = 0.0;
    double depthDb = 0.0;

    bool operator==(const ShapingZone &other) const
    {
        return lowMhz == other.lowMhz && highMhz == other.highMhz && depthDb == other.depthDb;
    }
};

// The zones as they are reported: "0-2 MHz @ 10 dB", comma separated.
std::string describeShapingZones(const std::vector<ShapingZone> &zones);

// The range a shaping filter's order and depth may be set to by hand. The order is
// the length of the error feedback every sample pays for, so it is held to what a
// stream can afford; the depth is how far above the bands the added noise may be
// pushed, and beyond 40 dB the filter's gain outside them is more than the
// quantiser's error clamp lets it use.
constexpr int kMinimumShapingOrder = 2;
constexpr int kMaximumShapingOrder = 64;
constexpr double kMaximumShapingDepthDb = 40.0;

// Shaping depths as a person types them, in dB: one, "40", or one for each stretch
// between the bands, low to high, "10, 40". Plain decimals, 0 to
// kMaximumShapingDepthDb, each with an optional "dB". Empty for anything else.
std::vector<double> parseShapingDepths(const std::string &text);

// The depths as they were read back: "10, 40 dB". Empty for none.
std::string describeShapingDepths(const std::vector<double> &depths);

// The stretches outside `bands`, from DC to the Nyquist limit of `sampleRateMhz`,
// low to high, each with the depth at the same place in `depths`: the first below
// the lowest band, the next between it and the one after, the last repeated where
// there are more stretches than depths, and any beyond the stretches unused here
// (more depths than stretches is refused before it gets this far). With bands
// 2-14 MHz at 35 Msps, "10, 40" puts 10 dB on the EFM below 2 MHz and 40 dB above
// 14 MHz.
std::vector<ShapingZone> gapShapingZones(const std::vector<FrequencyBand> &bands, const std::vector<double> &depths,
                                         double sampleRateMhz);

struct RequantizerSettings
{
    // The rate of the samples this is given.
    double sampleRateMhz = 40.0;

    int marginLevel = kDefaultMarginLevel;

    // Empty means defaultProtectedBands().
    std::vector<FrequencyBand> protectedBands;

    // How many of the sixteen bits the input carries: the converter's ten, less
    // the bit shift applied before this. A shifted sample's low bits are zero
    // already, and a drop is counted from the first one that is not.
    int inputBits = 10;

    // How the added noise is shaped.
    //
    //  - kFixed: one filter, designed at the start from the bands alone: low in
    //    them, shapingDepthDb higher outside. The added noise is the same
    //    everywhere in the bands, so the band's quietest slice decides for all of
    //    it, however much more noise the rest could take.
    //  - kAdaptive: designed again for every segment from the floor just
    //    measured, so that the added noise follows it (more where the band is
    //    already noisy, less where it is quiet) and as much as possible goes
    //    outside the bands, up to shapingDepthDb above the band's highest floor.
    //    The floor is also smoothed over a quarter of a megahertz before its low
    //    envelope is taken, which takes out the bias a minimum of noisy estimates
    //    has: about 1.2 dB too low on a flat floor.
    //
    // Fixed is the original design, and what is decided with it is what rfquant
    // decides. See useAdaptiveShaping() for the adaptive mode's own order and
    // depth.
    enum class Shaping { Fixed, Adaptive };
    Shaping shaping = Shaping::Fixed;
    int shapingOrder = 16;
    double shapingDepthDb = 10.0;

    // Stretches outside the bands with a depth of their own, in place of
    // shapingDepthDb; see ShapingZone. Where one overlaps a band the band wins,
    // and where two overlap the shallower does.
    std::vector<ShapingZone> shapingZones;

    // How far back the noise floor is estimated from.
    double historySeconds = 0.5;
};

// Switch `settings` to adaptive shaping, with the order and depth it was measured
// at: order 32, 20 dB. A finer filter is what lets the noise follow a sloping
// floor, and the wider depth what lets it be pushed out of the bands.
void useAdaptiveShaping(RequantizerSettings &settings);

// What a person asks of the requantiser, before it is turned into
// RequantizerSettings.
struct RequantizerRequest
{
    // The rate of the samples.
    double sampleRateMhz = 40.0;

    int marginLevel = kDefaultMarginLevel;

    // As asked for; cut at the Nyquist limit, and the default for the rate where
    // none is left.
    std::vector<FrequencyBand> bands;

    int inputBits = 10;
    bool adaptive = false;

    // Empty for the mode's own depth; one for everywhere outside the bands;
    // several for each stretch between them (gapShapingZones).
    std::vector<double> depthsDb;

    // 0 for the mode's own.
    int order = 0;
};

RequantizerSettings settingsFor(const RequantizerRequest &request);

// The bands `request` protects: those asked for that are left below the Nyquist
// limit, or the default for the rate.
std::vector<FrequencyBand> requestedBands(const RequantizerRequest &request);

// Why the depths in `request` cannot be used (more of them than there are
// stretches between the bands at the rate) in a sentence; empty when they can.
// One depth always can, and fewer than the stretches can too: the last stands for
// the rest.
std::string shapingDepthsProblem(const RequantizerRequest &request);

// What was done to one segment: how many of the input's low bits were dropped,
// whether the added noise was shaped, and what that cost the worst protected
// slice. A drop of 0 leaves the segment as it was.
struct RequantizerDecision
{
    int lsbDrop = 0;
    bool shaped = false;
    double degradationDb = 0.0;

    // What stopped it going further: the 1 MHz slice of the protected bands that
    // the next more aggressive setting would have raised the most, and that
    // slice's own floor in input LSBs. The median floor of the band says how noisy
    // the capture is; this says why that was not enough: one quiet slice, a notch
    // in the player's RF or the edge of a filter, holds the whole band back.
    // Zero-width when nothing was more aggressive to try.
    double limitLowMhz = 0.0;
    double limitHighMhz = 0.0;
    double limitFloorLsb = 0.0;

    // A(z) as it was applied, {1.0} for plain rounding; empty when nothing was
    // dropped. Adaptive shaping designs it afresh for every segment.
    std::vector<double> coefficients;
};

// A change of decision, from `firstSample` of the stream on.
struct RequantizationChange
{
    uint64_t firstSample = 0;
    int lsbDrop = 0;
    bool shaped = false;
};

// What a run of the requantiser comes to, for the report: the samples each choice
// was applied to, how many were shaped, the worst rise of any protected slice and
// the samples held at full scale.
class RequantizationStatus
{
public:
    struct Summary
    {
        uint64_t segments = 0;
        uint64_t samples = 0;
        // Only the changes: the first segment's decision, and every one after it
        // that differs from the one before.
        std::vector<RequantizationChange> changes;
        // Samples by bits dropped, index 0 for none.
        std::vector<uint64_t> samplesByDrop;
        uint64_t shapedSamples = 0;
        double worstDegradationDb = 0.0;
        uint64_t clipped = 0;
    };

    void record(const RequantizerDecision &decision, uint64_t firstSample, size_t count, uint64_t clipped);
    const Summary &summary() const { return m_summary; }

private:
    Summary m_summary;
    bool m_haveLast = false;
    int m_lastDrop = 0;
    bool m_lastShaped = false;
};

// The noise transfer function A(z), minimum phase, coefficients a[0..order] with
// a[0] = 1, whose |A(f)|^2 is low in `bands` and high outside them by about
// `depthDb`.
std::vector<double> designNoiseTransferFunction(const std::vector<FrequencyBand> &bands, double sampleRateMhz, double depthDb,
                                                int order);

// The same, high outside the bands by each zone's own depth where `zones` set one,
// and by `depthDb` everywhere else.
std::vector<double> designNoiseTransferFunction(const std::vector<FrequencyBand> &bands, const std::vector<ShapingZone> &zones,
                                                double sampleRateMhz, double depthDb, int order);

// |A(f)|^2.
double noiseTransferGainSquared(const std::vector<double> &coefficients, double frequencyMhz, double sampleRateMhz);

// Rounds to a coarser step, feeding the error back through A(z) when it has more
// than one coefficient. Signed 16-bit in and out; the output is a whole multiple
// of the step, saturated to the range.
class ShapingQuantizer
{
public:
    ShapingQuantizer() = default;

    // `outputBits` of the sixteen are kept. `coefficients` is A(z) as
    // designNoiseTransferFunction() returns it, or {1.0} for plain rounding.
    ShapingQuantizer(int outputBits, std::vector<double> coefficients);

    int16_t quantize(int32_t sample);

    // The same, over a block, in place.
    void quantizeInPlace(int16_t *samples, size_t count);

    // Forget the error history, as at the start of a stream.
    void reset();

    // Shape with another A(z) from the next sample on. The error history is kept
    // when the order is the same (adaptive shaping changes the filter between
    // segments of one stream, and the errors already made belong to it) and
    // forgotten when it is not.
    void setCoefficients(const std::vector<double> &coefficients);

    // A(z) as given, {1.0} for plain rounding.
    std::vector<double> coefficients() const;

    double step() const { return m_step; }
    uint64_t clipped() const { return m_clipped; }

private:
    double m_step = 1.0;
    double m_inverseStep = 1.0;
    double m_top = 32767.0;

    // a[1..order], and the error history held twice over so that the newest
    // `order` errors are always one contiguous window: no shuffle per sample.
    std::vector<double> m_feedback;
    std::vector<double> m_history;
    size_t m_position = 0;
    uint64_t m_clipped = 0;
};

class RfRequantizer
{
public:
    // The analysis transform, and how it is spent: frames of 1,024 samples,
    // averaged eight at a time into one spectrum, using one frame in four (the
    // floor moves slowly, and a quarter of the stream is plenty to follow it at a
    // quarter of the cost).
    static constexpr size_t kTransformSize = 1024;
    static constexpr size_t kBinCount = (kTransformSize / 2) + 1;
    static constexpr size_t kGroupFrames = 8;
    static constexpr size_t kFrameStride = 4;
    static constexpr size_t kGroupSpan = kTransformSize * kGroupFrames * kFrameStride;

    // One decision per segment: 2^20 samples, about 35 ms at 30 Msps, and a
    // whole number of every FLAC block size from 4,096 to 32,768, so a block never
    // straddles two settings and loses the zero bits of both.
    static constexpr size_t kSegmentSamples = size_t{1} << 20;

    explicit RfRequantizer(const RequantizerSettings &settings);

    // Decide what to do with one segment of at most kSegmentSamples, from the
    // floor including it, and do it in place when `apply` is set. A preview passes
    // false and pays only for the analysis.
    RequantizerDecision process(int16_t *samples, size_t count, bool apply);

    // The floor the last decision was made against, as the RMS of white noise of
    // the same power, in input LSBs; and the band's mean power over it.
    double noiseFloorLsb() const { return m_noiseFloorLsb; }
    double carrierToNoiseDb() const { return m_carrierToNoiseDb; }

    double limitDb() const { return m_limitDb; }
    const RequantizerSettings &settings() const { return m_settings; }
    const std::vector<FrequencyBand> &bands() const { return m_bands; }

    // Samples the requantisation held at full scale, over the whole run.
    uint64_t clipped() const;

private:
    struct Candidate
    {
        int lsbDrop = 0;
        bool shaped = false;
        double variance = 0.0;
        std::vector<double> gain;
        ShapingQuantizer quantizer;
    };

    // A candidate's cost: the most it raises any slice's floor, and which slice
    // that is, with the slice's mean floor power.
    struct SliceCost
    {
        double db = 0.0;
        double lowMhz = 0.0;
        double highMhz = 0.0;
        double floorPower = 0.0;
    };

    bool inBand(double frequencyMhz) const;
    void analyse(const int16_t *samples, size_t count);
    void periodogram(const double *frame, double *power);
    SliceCost degradation(const Candidate &candidate) const;

    // Adaptive shaping: A(z) designed from the floor just measured, and handed to
    // every shaped candidate with the gain it has in each bin.
    void redesignShaping();

    RequantizerSettings m_settings;
    std::vector<FrequencyBand> m_bands;
    double m_limitDb = 0.0;
    int m_holdSegments = 0;
    double m_inputLsb = 64.0;

    std::vector<Candidate> m_candidates;

    // Group spectra, newest last, as a ring of kBinCount floats each.
    std::vector<float> m_history;
    size_t m_historyGroups = 0;
    size_t m_historyFilled = 0;
    size_t m_historyPosition = 0;

    std::vector<double> m_floor;
    bool m_haveFloor = false;

    // Index into m_candidates of what is applied, m_candidates.size() for
    // nothing; how many segments in a row have justified something more
    // aggressive; and what was applied to the last segment, whose quantiser
    // carries its error into the next when it is applied again.
    size_t m_applied = 0;
    int m_pending = 0;
    size_t m_previous = 0;

    double m_noiseFloorLsb = 0.0;
    double m_carrierToNoiseDb = 0.0;

    std::vector<float> m_column;
    std::vector<double> m_frame;
    std::vector<double> m_spectrum;
    std::vector<double> m_group;
    std::vector<double> m_rawFloor;
    std::vector<double> m_smoothedFloor;

    // For adaptive shaping: cos and sin of each bin's frequency times each lag,
    // lag by lag, worked out once; the design and the gain it gives are then sums
    // over them.
    std::vector<double> m_lagCos;
    std::vector<double> m_lagSin;
    // And how far above the bands' highest floor each bin outside them may be
    // pushed, as a power ratio: the depth there, a zone's or the setting's.
    std::vector<double> m_outsideScale;
    // The fixed transform: bit-reversal order, twiddles and the Hann window,
    // worked out once.
    std::vector<size_t> m_bitReverse;
    std::vector<double> m_twiddleReal;
    std::vector<double> m_twiddleImaginary;
    std::vector<double> m_window;
    double m_windowPower = 0.0;
    std::vector<double> m_real;
    std::vector<double> m_imaginary;
};

} // namespace rfx
