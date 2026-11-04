/******************************************************************************
 * requantizer.cpp
 * rfx library - re-quantising a capture to as few bits as its own noise allows
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools. See requantizer.h.
 ******************************************************************************/

#include "requantizer.h"

#include <QString>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <utility>

namespace rfx {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The noise floor is a low percentile of each bin's history rather than its mean:
// the signal comes and goes, the floor is what is left when it has gone. Each
// group spectrum averages eight frames, so a bin's values follow a chi-squared
// distribution of sixteen degrees of freedom, whose tenth percentile is 0.58 of
// its mean and whose median is 0.959 of it; dividing by those turns either back
// into an estimate of the mean.
constexpr double kFloorPercentile = 0.10;
constexpr double kFloorPercentileCorrection = 0.58;
constexpr double kMedianCorrection = 0.959;

// Where the video carrier never leaves, the floor is taken from the quietest bin
// within this distance instead. Cautious: it can only under-estimate the floor,
// and so only make the decision more careful.
constexpr double kEnvelopeHalfWidthMhz = 2.0;

// Adaptive shaping only: the floor is averaged over this distance either side
// before its low envelope is taken. A minimum of noisy estimates sits below their
// mean (by about 13% on a flat floor, a 1.2 dB caution nobody chose) and an
// average over a quarter of a megahertz has too little noise left to do that,
// while a quiet place much narrower than a slice still shows.
constexpr double kFloorSmoothingHalfWidthMhz = 0.25;

// What useAdaptiveShaping() sets: measured, on a floor falling 7.5 dB across the
// top of the band, to drop a bit more than the fixed filter at the same margin.
constexpr int kAdaptiveShapingOrder = 32;
constexpr double kAdaptiveShapingDepthDb = 20.0;

// The slices the degradation is judged in.
constexpr double kSliceMhz = 1.0;

// The grid the shaping filter's weighting is integrated over.
constexpr size_t kDesignGrid = 4096;

// The fewest bits a segment can keep. See kMaximumRequantizerDrop.
constexpr int kMinimumAdaptiveBits = 4;

constexpr int kSampleBits = 16;
constexpr size_t kMinimumHistoryGroups = 8;
constexpr double kSmallestPower = 1e-12;
constexpr double kFullScale = 32768.0;

// Round to nearest, ties to even, by letting the floating-point unit do it: adding
// 1.5 x 2^52 leaves no bits for a fraction, so the sum is rounded in the default
// mode, and subtracting it again gives the rounded value. Exact for anything
// within +-2^51, which a 16-bit sample is far inside. Written out because
// std::nearbyint is a library call that saves and restores the floating-point
// environment on some platforms, and cost thirty-odd nanoseconds a sample, most of
// what requantising a stream costs.
constexpr double kRoundingConstant = 6755399441055744.0;

inline double roundToNearestEven(double value)
{
    return (value + kRoundingConstant) - kRoundingConstant;
}

struct MarginLevel
{
    const char *name;
    double limitDb;
    double holdSeconds;
};

// Aggressive takes any reduction at once; each level after it allows half as much
// degradation, or less, and waits longer before reducing.
constexpr std::array<MarginLevel, 5> kMarginLevels{{
    {"aggressive", 1.0, 0.0},
    {"moderate", 0.5, 0.1},
    {"safe", 0.2, 0.25},
    {"extra safe", 0.1, 0.5},
    {"ultra safe", 0.05, 1.0},
}};

const MarginLevel &level(int index)
{
    return kMarginLevels[static_cast<size_t>(std::clamp(index, kMinimumMarginLevel, kMaximumMarginLevel))];
}

bool inBands(const std::vector<FrequencyBand> &bands, double frequencyMhz)
{
    return std::any_of(bands.begin(), bands.end(),
                       [frequencyMhz](const FrequencyBand &band) { return frequencyMhz >= band.lowMhz && frequencyMhz <= band.highMhz; });
}

// The depth outside the bands at `frequencyMhz`: the shallowest of the zones that
// hold it, or `depthDb` where none does.
double depthAt(const std::vector<ShapingZone> &zones, double depthDb, double frequencyMhz)
{
    std::optional<double> zoned;
    for (const ShapingZone &zone : zones) {
        if (frequencyMhz >= zone.lowMhz && frequencyMhz <= zone.highMhz) {
            zoned = std::min(zoned.value_or(zone.depthDb), zone.depthDb);
        }
    }
    return zoned.value_or(depthDb);
}

// A(z), minimum phase with a[0] = 1, from the autocorrelation of the wanted noise
// weighting, by Levinson-Durbin: |A|^2 comes out proportional to 1/weight.
std::vector<double> levinsonDurbin(const std::vector<double> &correlation)
{
    const size_t size = correlation.empty() ? 0 : correlation.size() - 1;
    std::vector<double> coefficients(size + 1, 0.0);
    coefficients[0] = 1.0;
    if (size == 0) {
        return coefficients;
    }
    double error = correlation[0] * (1.0 + 1e-9);
    for (size_t i = 1; i <= size; ++i) {
        double accumulated = correlation[i];
        for (size_t j = 1; j < i; ++j) {
            accumulated += coefficients[j] * correlation[i - j];
        }
        const double reflection = -accumulated / error;
        const std::vector<double> previous = coefficients;
        for (size_t j = 1; j < i; ++j) {
            coefficients[j] = previous[j] + (reflection * previous[i - j]);
        }
        coefficients[i] = reflection;
        error *= 1.0 - (reflection * reflection);
    }
    return coefficients;
}

// To the hundredth, without the zeros: "13.5", "2".
std::string shortDecimal(double value)
{
    std::string text = formatDecimal(value, 2);
    while (text.find('.') != std::string::npos && (text.back() == '0' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

// `text` without its blanks, in lower case: what both parsers read.
std::string compact(const std::string &text)
{
    std::string out;
    for (const char character : text) {
        if (character != ' ' && character != '\t') {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
    }
    return out;
}

bool startsWith(const std::string &text, size_t at, const char *prefix)
{
    return text.compare(at, std::char_traits<char>::length(prefix), prefix) == 0;
}

// A plain decimal at `at` in `text`: digits, and optionally a point and more
// digits. By hand rather than through strtod, which reads "13.5" as 13 in a locale
// whose decimal separator is a comma. `at` moves past it.
std::optional<double> takeDecimal(const std::string &text, size_t &at)
{
    const auto isDigit = [&text](size_t index) { return index < text.size() && text[index] >= '0' && text[index] <= '9'; };

    size_t length = at;
    bool anyDigit = false;
    double value = 0.0;
    while (isDigit(length)) {
        value = (value * 10.0) + static_cast<double>(text[length] - '0');
        anyDigit = true;
        ++length;
    }
    if (length < text.size() && text[length] == '.') {
        ++length;
        double scale = 0.1;
        while (isDigit(length)) {
            value += scale * static_cast<double>(text[length] - '0');
            scale /= 10.0;
            anyDigit = true;
            ++length;
        }
    }
    if (!anyDigit) {
        return std::nullopt;
    }
    at = length;
    return value;
}

} // namespace

std::string formatDecimal(double value, int decimals)
{
    return QString::number(value, 'f', decimals).toStdString();
}

void useAdaptiveShaping(RequantizerSettings &settings)
{
    settings.shaping = RequantizerSettings::Shaping::Adaptive;
    settings.shapingOrder = kAdaptiveShapingOrder;
    settings.shapingDepthDb = kAdaptiveShapingDepthDb;
}

double marginLimitDb(int index)
{
    return level(index).limitDb;
}

double marginHoldSeconds(int index)
{
    return level(index).holdSeconds;
}

const char *marginLevelName(int index)
{
    return level(index).name;
}

std::vector<FrequencyBand> defaultProtectedBands(double sampleRateMhz)
{
    return {{0.0, std::min(14.0, (sampleRateMhz / 2.0) - 1.5)}};
}

std::string describeBands(const std::vector<FrequencyBand> &bands)
{
    std::string text;
    for (const FrequencyBand &band : bands) {
        if (!text.empty()) {
            text += ", ";
        }
        text += shortDecimal(band.lowMhz) + "-" + shortDecimal(band.highMhz);
    }
    return text.empty() ? text : text + " MHz";
}

std::string describeShapingZones(const std::vector<ShapingZone> &zones)
{
    std::string text;
    for (const ShapingZone &zone : zones) {
        if (!text.empty()) {
            text += ", ";
        }
        text += shortDecimal(zone.lowMhz) + "-" + shortDecimal(zone.highMhz) + " MHz @ " + shortDecimal(zone.depthDb) + " dB";
    }
    return text;
}

std::vector<FrequencyBand> parseBands(const std::string &text)
{
    std::string spelled = compact(text);
    if (spelled.size() >= 3 && spelled.compare(spelled.size() - 3, 3, "mhz") == 0) {
        spelled.resize(spelled.size() - 3);
    }

    size_t at = 0;
    std::vector<FrequencyBand> bands;
    for (;;) {
        const std::optional<double> low = takeDecimal(spelled, at);
        if (!low.has_value() || at >= spelled.size() || (spelled[at] != '-' && spelled[at] != ':')) {
            return {};
        }
        ++at;
        const std::optional<double> high = takeDecimal(spelled, at);
        if (!high.has_value() || *high <= *low) {
            return {};
        }
        bands.push_back({*low, *high});

        if (at >= spelled.size()) {
            return bands;
        }
        if (spelled[at] != ',') {
            return {};
        }
        ++at;
    }
}

std::vector<FrequencyBand> bandsWithin(const std::vector<FrequencyBand> &bands, double sampleRateMhz)
{
    const double nyquist = sampleRateMhz / 2.0;
    std::vector<FrequencyBand> usable;
    for (const FrequencyBand &band : bands) {
        const double high = std::min(band.highMhz, nyquist);
        if (band.lowMhz < high) {
            usable.push_back({band.lowMhz, high});
        }
    }
    return usable;
}

std::vector<double> parseShapingDepths(const std::string &text)
{
    const std::string spelled = compact(text);
    size_t at = 0;
    std::vector<double> depths;
    for (;;) {
        const std::optional<double> depth = takeDecimal(spelled, at);
        if (!depth.has_value() || *depth > kMaximumShapingDepthDb) {
            return {};
        }
        if (startsWith(spelled, at, "db")) {
            at += 2;
        }
        depths.push_back(*depth);

        if (at >= spelled.size()) {
            return depths;
        }
        if (spelled[at] != ',') {
            return {};
        }
        ++at;
    }
}

std::string describeShapingDepths(const std::vector<double> &depths)
{
    std::string text;
    for (const double depth : depths) {
        if (!text.empty()) {
            text += ", ";
        }
        text += shortDecimal(depth);
    }
    return text.empty() ? text : text + " dB";
}

std::vector<ShapingZone> gapShapingZones(const std::vector<FrequencyBand> &bands, const std::vector<double> &depths,
                                         double sampleRateMhz)
{
    if (depths.empty()) {
        return {};
    }
    std::vector<FrequencyBand> sorted = bandsWithin(bands, sampleRateMhz);
    std::sort(sorted.begin(), sorted.end(), [](const FrequencyBand &left, const FrequencyBand &right) { return left.lowMhz < right.lowMhz; });

    // Each stretch nothing protects, from DC up, takes the next depth.
    std::vector<ShapingZone> zones;
    const auto add = [&zones, &depths](double low, double high) {
        if (high > low) {
            const size_t index = std::min(zones.size(), depths.size() - 1);
            zones.push_back({low, high, std::clamp(depths[index], 0.0, kMaximumShapingDepthDb)});
        }
    };
    double edge = 0.0;
    for (const FrequencyBand &band : sorted) {
        add(edge, band.lowMhz);
        edge = std::max(edge, band.highMhz);
    }
    add(edge, sampleRateMhz / 2.0);
    return zones;
}

std::vector<FrequencyBand> requestedBands(const RequantizerRequest &request)
{
    std::vector<FrequencyBand> bands = bandsWithin(request.bands, request.sampleRateMhz);
    return bands.empty() ? defaultProtectedBands(request.sampleRateMhz) : bands;
}

RequantizerSettings settingsFor(const RequantizerRequest &request)
{
    RequantizerSettings settings;
    settings.sampleRateMhz = request.sampleRateMhz;
    settings.marginLevel = request.marginLevel;
    settings.protectedBands = requestedBands(request);
    settings.inputBits = request.inputBits;
    if (request.adaptive) {
        useAdaptiveShaping(settings);
    }

    // A depth or an order asked for, in place of the mode's own. One depth is the
    // depth everywhere outside the bands; several are one for each stretch between
    // them, from DC up, the last of them standing for the rest.
    const std::vector<double> &depths = request.depthsDb;
    if (!depths.empty()) {
        settings.shapingDepthDb = std::clamp(depths.back(), 0.0, kMaximumShapingDepthDb);
    }
    if (depths.size() > 1) {
        settings.shapingZones = gapShapingZones(settings.protectedBands, depths, settings.sampleRateMhz);
    }
    if (request.order > 0) {
        settings.shapingOrder = std::clamp(request.order, kMinimumShapingOrder, kMaximumShapingOrder);
    }
    return settings;
}

std::string shapingDepthsProblem(const RequantizerRequest &request)
{
    const size_t depths = request.depthsDb.size();
    if (depths <= 1) {
        return {};
    }
    const std::vector<ShapingZone> stretches = gapShapingZones(requestedBands(request), {0.0}, request.sampleRateMhz);
    if (depths <= stretches.size()) {
        return {};
    }

    const std::string rate = shortDecimal(request.sampleRateMhz);
    if (stretches.empty()) {
        return std::to_string(depths) + " shaping depths, but at " + rate + " Msps the protected bands leave nothing outside them: give one depth.";
    }
    std::vector<FrequencyBand> where;
    for (const ShapingZone &stretch : stretches) {
        where.push_back({stretch.lowMhz, stretch.highMhz});
    }
    const std::string most = std::to_string(stretches.size());
    return std::to_string(depths) + " shaping depths, but at " + rate + " Msps the protected bands leave " + most
        + (stretches.size() == 1 ? " stretch" : " stretches") + " outside them (" + describeBands(where) + "): give at most " + most + ".";
}

void RequantizationStatus::record(const RequantizerDecision &decision, uint64_t firstSample, size_t count, uint64_t clipped)
{
    Summary &s = m_summary;
    if (!m_haveLast || m_lastDrop != decision.lsbDrop || m_lastShaped != decision.shaped) {
        s.changes.push_back({firstSample, decision.lsbDrop, decision.shaped});
    }
    m_haveLast = true;
    m_lastDrop = decision.lsbDrop;
    m_lastShaped = decision.shaped;

    ++s.segments;
    s.samples += count;

    const auto drop = static_cast<size_t>(std::max(decision.lsbDrop, 0));
    if (s.samplesByDrop.size() <= drop) {
        s.samplesByDrop.resize(drop + 1, 0);
    }
    s.samplesByDrop[drop] += count;
    if (decision.shaped) {
        s.shapedSamples += count;
    }
    s.worstDegradationDb = std::max(s.worstDegradationDb, decision.degradationDb);
    s.clipped = clipped;
}

std::vector<double> designNoiseTransferFunction(const std::vector<FrequencyBand> &bands, double sampleRateMhz, double depthDb, int order)
{
    return designNoiseTransferFunction(bands, {}, sampleRateMhz, depthDb, order);
}

std::vector<double> designNoiseTransferFunction(const std::vector<FrequencyBand> &bands, const std::vector<ShapingZone> &zones,
                                                double sampleRateMhz, double depthDb, int order)
{
    const auto size = static_cast<size_t>(std::max(order, 0));

    // The weighting on the design grid: 1 in the bands, the depth below it outside
    // them (a zone's own, or the one given).
    std::vector<double> weights(kDesignGrid);
    for (size_t index = 0; index < kDesignGrid; ++index) {
        const double frequency = (static_cast<double>(index) + 0.5) * (sampleRateMhz / 2.0) / static_cast<double>(kDesignGrid);
        weights[index] = inBands(bands, frequency) ? 1.0 : std::pow(10.0, -depthAt(zones, depthDb, frequency) / 10.0);
    }

    // Its autocorrelation.
    std::vector<double> correlation(size + 1, 0.0);
    for (size_t lag = 0; lag <= size; ++lag) {
        double sum = 0.0;
        for (size_t index = 0; index < kDesignGrid; ++index) {
            const double frequency = (static_cast<double>(index) + 0.5) * (sampleRateMhz / 2.0) / static_cast<double>(kDesignGrid);
            const double weight = weights[index];
            sum += weight * std::cos(2.0 * kPi * frequency / sampleRateMhz * static_cast<double>(lag));
        }
        correlation[lag] = sum / static_cast<double>(kDesignGrid);
    }
    return levinsonDurbin(correlation);
}

double noiseTransferGainSquared(const std::vector<double> &coefficients, double frequencyMhz, double sampleRateMhz)
{
    const double omega = 2.0 * kPi * frequencyMhz / sampleRateMhz;
    double real = 0.0;
    double imaginary = 0.0;
    for (size_t k = 0; k < coefficients.size(); ++k) {
        real += coefficients[k] * std::cos(omega * static_cast<double>(k));
        imaginary -= coefficients[k] * std::sin(omega * static_cast<double>(k));
    }
    return (real * real) + (imaginary * imaginary);
}

// --- ShapingQuantizer -------------------------------------------------------

ShapingQuantizer::ShapingQuantizer(int outputBits, std::vector<double> coefficients)
    : m_step(std::ldexp(1.0, kSampleBits - outputBits))
    , m_inverseStep(1.0 / m_step)
    , m_top(kFullScale - m_step)
{
    if (coefficients.size() > 1) {
        m_feedback.assign(coefficients.begin() + 1, coefficients.end());
    }
    m_history.assign(2 * m_feedback.size(), 0.0);
}

int16_t ShapingQuantizer::quantize(int32_t sample)
{
    const size_t order = m_feedback.size();
    double value = static_cast<double>(sample);

    if (order > 0) {
        // The error history's contribution, the newest last. Everything but the
        // newest term is known before the previous sample has been rounded, and
        // summing it first in four independent partial sums keeps the chain from
        // one sample to the next down to a multiply and a rounding, which is what
        // lets this keep up with a live stream.
        const double *const errors = m_history.data() + m_position;
        std::array<double, 4> partial{};
        size_t k = 1;
        for (; k + 3 < order; k += 4) {
            partial[0] += m_feedback[k] * errors[k];
            partial[1] += m_feedback[k + 1] * errors[k + 1];
            partial[2] += m_feedback[k + 2] * errors[k + 2];
            partial[3] += m_feedback[k + 3] * errors[k + 3];
        }
        for (; k < order; ++k) {
            partial[0] += m_feedback[k] * errors[k];
        }
        value = (value + ((partial[0] + partial[1]) + (partial[2] + partial[3]))) + (m_feedback[0] * errors[0]);
    }

    // Round to nearest, ties to even: no DC bias.
    double quantised = roundToNearestEven(value * m_inverseStep) * m_step;
    if (quantised < -kFullScale) {
        quantised = -kFullScale;
        ++m_clipped;
    } else if (quantised > m_top) {
        quantised = m_top;
        ++m_clipped;
    }

    if (order > 0) {
        // Held to one step after a clip, or the feedback runs away with it.
        const double error = std::clamp(quantised - value, -m_step, m_step);
        m_position = m_position == 0 ? order - 1 : m_position - 1;
        m_history[m_position] = error;
        m_history[m_position + order] = error;
    }
    return static_cast<int16_t>(quantised);
}

void ShapingQuantizer::quantizeInPlace(int16_t *samples, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        samples[index] = quantize(samples[index]);
    }
}

void ShapingQuantizer::reset()
{
    std::fill(m_history.begin(), m_history.end(), 0.0);
    m_position = 0;
}

void ShapingQuantizer::setCoefficients(const std::vector<double> &coefficients)
{
    const size_t order = coefficients.empty() ? 0 : coefficients.size() - 1;
    if (order != m_feedback.size()) {
        m_feedback.assign(order, 0.0);
        m_history.assign(2 * order, 0.0);
        m_position = 0;
    }
    std::copy(coefficients.begin() + (coefficients.empty() ? 0 : 1), coefficients.end(), m_feedback.begin());
}

std::vector<double> ShapingQuantizer::coefficients() const
{
    std::vector<double> all{1.0};
    all.insert(all.end(), m_feedback.begin(), m_feedback.end());
    return all;
}

// --- RfRequantizer ----------------------------------------------------------

RfRequantizer::RfRequantizer(const RequantizerSettings &settings)
    : m_settings(settings)
    , m_bands(settings.protectedBands.empty() ? defaultProtectedBands(settings.sampleRateMhz) : settings.protectedBands)
    , m_limitDb(marginLimitDb(settings.marginLevel))
{
    const double samplesPerSecond = m_settings.sampleRateMhz * 1e6;
    m_holdSegments = static_cast<int>(std::ceil(marginHoldSeconds(m_settings.marginLevel) * samplesPerSecond / static_cast<double>(kSegmentSamples)));

    const int inputBits = std::clamp(m_settings.inputBits, kMinimumAdaptiveBits + 1, kSampleBits);
    m_inputLsb = std::ldexp(1.0, kSampleBits - inputBits);

    // The candidates, most aggressive first; at equal bits the unshaped one first,
    // because it is the smaller file and is taken whenever it passes.
    const std::vector<double> shaping = designNoiseTransferFunction(m_bands, m_settings.shapingZones, m_settings.sampleRateMhz,
                                                                    m_settings.shapingDepthDb, m_settings.shapingOrder);
    const double binWidth = m_settings.sampleRateMhz / static_cast<double>(kTransformSize);
    for (int bits = std::max(inputBits - kMaximumRequantizerDrop, kMinimumAdaptiveBits); bits < inputBits; ++bits) {
        for (const bool shaped : {false, true}) {
            Candidate candidate;
            candidate.lsbDrop = inputBits - bits;
            candidate.shaped = shaped;
            candidate.quantizer = ShapingQuantizer(bits, shaped ? shaping : std::vector<double>{1.0});

            // The variance a requantisation of an integer signal adds.
            const double step = candidate.quantizer.step();
            candidate.variance = ((step * step) + (2.0 * m_inputLsb * m_inputLsb)) / 12.0;

            candidate.gain.resize(kBinCount);
            for (size_t bin = 0; bin < kBinCount; ++bin) {
                candidate.gain[bin] =
                    shaped ? noiseTransferGainSquared(shaping, static_cast<double>(bin) * binWidth, m_settings.sampleRateMhz) : 1.0;
            }
            m_candidates.push_back(std::move(candidate));
        }
    }
    m_applied = m_candidates.size();
    m_previous = m_candidates.size();

    // Adaptive shaping starts from the fixed design above, which stands until there
    // is a floor to design from.
    if (m_settings.shaping == RequantizerSettings::Shaping::Adaptive) {
        const auto lags = static_cast<size_t>(std::max(m_settings.shapingOrder, 0));
        m_lagCos.resize((lags + 1) * kBinCount);
        m_lagSin.resize((lags + 1) * kBinCount);
        for (size_t lag = 0; lag <= lags; ++lag) {
            for (size_t bin = 0; bin < kBinCount; ++bin) {
                const double angle = 2.0 * kPi * static_cast<double>(bin * lag) / static_cast<double>(kTransformSize);
                m_lagCos[(lag * kBinCount) + bin] = std::cos(angle);
                m_lagSin[(lag * kBinCount) + bin] = std::sin(angle);
            }
        }
        m_outsideScale.resize(kBinCount);
        for (size_t bin = 0; bin < kBinCount; ++bin) {
            m_outsideScale[bin] = std::pow(10.0, depthAt(m_settings.shapingZones, m_settings.shapingDepthDb, static_cast<double>(bin) * binWidth) / 10.0);
        }
    }

    m_historyGroups = std::max(kMinimumHistoryGroups, static_cast<size_t>(std::ceil(m_settings.historySeconds * samplesPerSecond / static_cast<double>(kGroupSpan))));
    m_history.assign(m_historyGroups * kBinCount, 0.0F);
    m_column.resize(m_historyGroups);

    m_floor.assign(kBinCount, kSmallestPower);
    m_frame.resize(kTransformSize);
    m_spectrum.resize(kBinCount);
    m_group.resize(kBinCount);
    m_rawFloor.resize(kBinCount);
    m_real.resize(kTransformSize);
    m_imaginary.resize(kTransformSize);

    // The transform's tables, once.
    size_t levels = 0;
    while ((size_t{1} << levels) < kTransformSize) {
        ++levels;
    }
    m_bitReverse.resize(kTransformSize);
    m_window.resize(kTransformSize);
    for (size_t i = 0; i < kTransformSize; ++i) {
        size_t reversed = 0;
        for (size_t bit = 0; bit < levels; ++bit) {
            if ((i & (size_t{1} << bit)) != 0) {
                reversed |= size_t{1} << (levels - 1 - bit);
            }
        }
        m_bitReverse[i] = reversed;
        m_window[i] = 0.5 - (0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kTransformSize)));
        m_windowPower += m_window[i] * m_window[i];
    }
    m_twiddleReal.resize(kTransformSize / 2);
    m_twiddleImaginary.resize(kTransformSize / 2);
    for (size_t i = 0; i < kTransformSize / 2; ++i) {
        const double angle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(kTransformSize);
        m_twiddleReal[i] = std::cos(angle);
        m_twiddleImaginary[i] = -std::sin(angle);
    }
}

bool RfRequantizer::inBand(double frequencyMhz) const
{
    return inBands(m_bands, frequencyMhz);
}

void RfRequantizer::periodogram(const double *frame, double *power)
{
    for (size_t i = 0; i < kTransformSize; ++i) {
        m_real[m_bitReverse[i]] = frame[i] * m_window[i];
        m_imaginary[m_bitReverse[i]] = 0.0;
    }
    for (size_t length = 2; length <= kTransformSize; length <<= 1) {
        const size_t half = length / 2;
        const size_t stride = kTransformSize / length;
        for (size_t start = 0; start < kTransformSize; start += length) {
            for (size_t j = 0; j < half; ++j) {
                const double twiddleReal = m_twiddleReal[j * stride];
                const double twiddleImaginary = m_twiddleImaginary[j * stride];
                const size_t upper = start + j + half;
                const double productReal = (m_real[upper] * twiddleReal) - (m_imaginary[upper] * twiddleImaginary);
                const double productImaginary = (m_real[upper] * twiddleImaginary) + (m_imaginary[upper] * twiddleReal);
                m_real[upper] = m_real[start + j] - productReal;
                m_imaginary[upper] = m_imaginary[start + j] - productImaginary;
                m_real[start + j] += productReal;
                m_imaginary[start + j] += productImaginary;
            }
        }
    }
    // Scaled so that the two-sided mean is the variance.
    for (size_t bin = 0; bin < kBinCount; ++bin) {
        power[bin] = ((m_real[bin] * m_real[bin]) + (m_imaginary[bin] * m_imaginary[bin])) / m_windowPower;
    }
}

void RfRequantizer::analyse(const int16_t *samples, size_t count)
{
    const double binWidth = m_settings.sampleRateMhz / static_cast<double>(kTransformSize);
    double bandPower = 0.0;
    size_t bandBins = 0;
    size_t groupsNow = 0;

    for (size_t start = 0; start + kGroupSpan <= count; start += kGroupSpan) {
        std::fill(m_group.begin(), m_group.end(), 0.0);
        for (size_t frame = 0; frame < kGroupFrames; ++frame) {
            const int16_t *const source = samples + start + (frame * kTransformSize * kFrameStride);
            for (size_t i = 0; i < kTransformSize; ++i) {
                m_frame[i] = static_cast<double>(source[i]);
            }
            periodogram(m_frame.data(), m_spectrum.data());
            for (size_t bin = 0; bin < kBinCount; ++bin) {
                m_group[bin] += m_spectrum[bin] / static_cast<double>(kGroupFrames);
            }
        }

        float *const slot = m_history.data() + (m_historyPosition * kBinCount);
        for (size_t bin = 0; bin < kBinCount; ++bin) {
            slot[bin] = static_cast<float>(m_group[bin]);
            if (inBand(static_cast<double>(bin) * binWidth)) {
                bandPower += m_group[bin];
                ++bandBins;
            }
        }
        m_historyPosition = (m_historyPosition + 1) % m_historyGroups;
        m_historyFilled = std::min(m_historyFilled + 1, m_historyGroups);
        ++groupsNow;
    }

    // Too short to hold a group: the end of a stream. The floor stands.
    if (bandBins == 0) {
        return;
    }

    const auto percentileIndex = static_cast<size_t>(kFloorPercentile * static_cast<double>(m_historyFilled - 1));
    for (size_t bin = 0; bin < kBinCount; ++bin) {
        for (size_t group = 0; group < m_historyFilled; ++group) {
            m_column[group] = m_history[(group * kBinCount) + bin];
        }
        std::nth_element(m_column.begin(), m_column.begin() + static_cast<std::ptrdiff_t>(percentileIndex),
                         m_column.begin() + static_cast<std::ptrdiff_t>(m_historyFilled));
        double estimate = static_cast<double>(m_column[percentileIndex]) / kFloorPercentileCorrection;

        // This segment on its own, so that a floor that falls abruptly is followed
        // at once rather than after the history has caught up with it.
        if (groupsNow < m_historyFilled) {
            for (size_t group = 0; group < groupsNow; ++group) {
                const size_t index = (m_historyPosition + m_historyGroups - 1 - group) % m_historyGroups;
                m_column[group] = m_history[(index * kBinCount) + bin];
            }
            const size_t middle = groupsNow / 2;
            std::nth_element(m_column.begin(), m_column.begin() + static_cast<std::ptrdiff_t>(middle),
                             m_column.begin() + static_cast<std::ptrdiff_t>(groupsNow));
            const double current = static_cast<double>(m_column[middle]) / kMedianCorrection;
            estimate = std::min(estimate, current);
        }
        m_rawFloor[bin] = estimate;
    }

    const auto bins = static_cast<std::ptrdiff_t>(kBinCount);

    // Adaptive shaping smooths the estimates first, within the bands, so that the
    // envelope below takes the minimum of a floor rather than of its noise.
    if (m_settings.shaping == RequantizerSettings::Shaping::Adaptive) {
        const auto reach = static_cast<std::ptrdiff_t>(std::ceil(kFloorSmoothingHalfWidthMhz / binWidth));
        m_smoothedFloor.resize(kBinCount);
        for (std::ptrdiff_t bin = 0; bin < bins; ++bin) {
            const bool inside = inBand(static_cast<double>(bin) * binWidth);
            double sum = 0.0;
            size_t counted = 0;
            for (std::ptrdiff_t other = std::max<std::ptrdiff_t>(0, bin - reach); other <= std::min(bins - 1, bin + reach); ++other) {
                const auto index = static_cast<size_t>(other);
                if (!inside || inBand(static_cast<double>(index) * binWidth)) {
                    sum += m_rawFloor[index];
                    ++counted;
                }
            }
            m_smoothedFloor[static_cast<size_t>(bin)] = sum / static_cast<double>(counted);
        }
        m_rawFloor.swap(m_smoothedFloor);
    }

    // The low envelope, within the bands.
    const auto halfWidth = static_cast<std::ptrdiff_t>(std::ceil(kEnvelopeHalfWidthMhz / binWidth));
    for (std::ptrdiff_t bin = 0; bin < bins; ++bin) {
        double lowest = m_rawFloor[static_cast<size_t>(bin)];
        for (std::ptrdiff_t other = std::max<std::ptrdiff_t>(0, bin - halfWidth); other <= std::min(bins - 1, bin + halfWidth); ++other) {
            const auto index = static_cast<size_t>(other);
            if (inBand(static_cast<double>(index) * binWidth)) {
                lowest = std::min(lowest, m_rawFloor[index]);
            }
        }
        m_floor[static_cast<size_t>(bin)] = std::max(lowest, kSmallestPower);
    }
    m_haveFloor = true;

    // What the floor and the band amount to, for whoever is watching.
    std::vector<double> inBandFloor;
    for (size_t bin = 0; bin < kBinCount; ++bin) {
        if (inBand(static_cast<double>(bin) * binWidth)) {
            inBandFloor.push_back(m_floor[bin]);
        }
    }
    if (!inBandFloor.empty()) {
        const size_t middle = inBandFloor.size() / 2;
        std::nth_element(inBandFloor.begin(), inBandFloor.begin() + static_cast<std::ptrdiff_t>(middle), inBandFloor.end());
        const double median = inBandFloor[middle];
        m_noiseFloorLsb = std::sqrt(median) / m_inputLsb;
        m_carrierToNoiseDb = 10.0 * std::log10((bandPower / static_cast<double>(bandBins)) / median);
    }
}

RfRequantizer::SliceCost RfRequantizer::degradation(const Candidate &candidate) const
{
    const double binWidth = m_settings.sampleRateMhz / static_cast<double>(kTransformSize);
    SliceCost worst;
    for (const FrequencyBand &band : m_bands) {
        // Counted rather than stepped in floating point, so that no slice is gained
        // or lost to rounding at the top of a band.
        const auto slices = static_cast<int>(std::ceil((band.highMhz - band.lowMhz - 1e-9) / kSliceMhz));
        for (int slice = 0; slice < slices; ++slice) {
            const double low = band.lowMhz + (static_cast<double>(slice) * kSliceMhz);
            const double high = std::min(low + kSliceMhz, band.highMhz);
            double added = 0.0;
            double floor = 0.0;
            size_t bins = 0;
            for (size_t bin = 0; bin < kBinCount; ++bin) {
                const double frequency = static_cast<double>(bin) * binWidth;
                if (frequency >= low && frequency < high) {
                    added += candidate.variance * candidate.gain[bin];
                    floor += m_floor[bin];
                    ++bins;
                }
            }
            if (floor > 0.0) {
                const double cost = 10.0 * std::log10(1.0 + (added / floor));
                if (cost > worst.db) {
                    worst = {cost, low, high, floor / static_cast<double>(bins)};
                }
            }
        }
    }
    return worst;
}

void RfRequantizer::redesignShaping()
{
    const double binWidth = m_settings.sampleRateMhz / static_cast<double>(kTransformSize);

    // The wanted noise: the floor itself in the bands, so that every slice of them
    // is raised by the same proportion; and outside them, the depth above the
    // bands' highest floor (a zone's own where one is set) so that as much as can
    // go there does.
    double highest = 0.0;
    for (size_t bin = 0; bin < kBinCount; ++bin) {
        if (inBand(static_cast<double>(bin) * binWidth)) {
            highest = std::max(highest, m_floor[bin]);
        }
    }
    if (highest <= kSmallestPower) {
        return;
    }

    // Its weighting's autocorrelation, and A(z) from that as the fixed design does
    // it, over the analysis bins rather than a finer grid, because the floor is
    // known at no finer a resolution.
    const size_t lags = m_lagCos.size() / kBinCount;
    std::vector<double> correlation(lags, 0.0);
    for (size_t bin = 0; bin < kBinCount; ++bin) {
        const double target = inBand(static_cast<double>(bin) * binWidth) ? std::max(m_floor[bin], kSmallestPower) : highest * m_outsideScale[bin];
        const double weight = 1.0 / target;
        for (size_t lag = 0; lag < lags; ++lag) {
            correlation[lag] += weight * m_lagCos[(lag * kBinCount) + bin];
        }
    }
    const std::vector<double> shaping = levinsonDurbin(correlation);

    // |A|^2 in every bin, once for all the shaped candidates.
    std::vector<double> gain(kBinCount);
    for (size_t bin = 0; bin < kBinCount; ++bin) {
        double real = 0.0;
        double imaginary = 0.0;
        for (size_t k = 0; k < shaping.size(); ++k) {
            real += shaping[k] * m_lagCos[(k * kBinCount) + bin];
            imaginary -= shaping[k] * m_lagSin[(k * kBinCount) + bin];
        }
        gain[bin] = (real * real) + (imaginary * imaginary);
    }

    for (Candidate &candidate : m_candidates) {
        if (candidate.shaped) {
            candidate.gain = gain;
            candidate.quantizer.setCoefficients(shaping);
        }
    }
}

RequantizerDecision RfRequantizer::process(int16_t *samples, size_t count, bool apply)
{
    analyse(samples, count);
    if (m_settings.shaping == RequantizerSettings::Shaping::Adaptive && m_haveFloor) {
        redesignShaping();
    }

    const size_t none = m_candidates.size();
    size_t wanted = none;
    if (m_haveFloor) {
        for (size_t index = 0; index < none; ++index) {
            if (degradation(m_candidates[index]).db <= m_limitDb) {
                wanted = index;
                break;
            }
        }
    }

    // More careful at once; more aggressive only once it has been justified for
    // longer than the margin's hold, so that a passing lull does not cost the next
    // loud moment its bits.
    if (wanted >= m_applied || ++m_pending > m_holdSegments) {
        m_applied = wanted;
        m_pending = 0;
    }
    if (m_applied < none && degradation(m_candidates[m_applied]).db > m_limitDb) {
        m_applied = wanted;
    }

    RequantizerDecision decision;

    // Why it went no further: the slice the next more aggressive candidate would
    // have raised most. Candidates run most aggressive first, so that is the one
    // just before the decision, or the last of all when nothing passed.
    if (m_haveFloor && m_applied > 0) {
        const SliceCost limit = degradation(m_candidates[m_applied - 1]);
        decision.limitLowMhz = limit.lowMhz;
        decision.limitHighMhz = limit.highMhz;
        decision.limitFloorLsb = std::sqrt(limit.floorPower) / m_inputLsb;
    }

    if (m_applied < none) {
        Candidate &candidate = m_candidates[m_applied];
        decision.lsbDrop = candidate.lsbDrop;
        decision.shaped = candidate.shaped;
        decision.degradationDb = degradation(candidate).db;
        decision.coefficients = candidate.quantizer.coefficients();

        if (apply) {
            // A quantiser taken over from another carries an error history that
            // belongs to that one's shaping.
            if (m_applied != m_previous) {
                candidate.quantizer.reset();
            }
            candidate.quantizer.quantizeInPlace(samples, count);
        }
    }
    m_previous = m_applied;
    return decision;
}

uint64_t RfRequantizer::clipped() const
{
    uint64_t total = 0;
    for (const Candidate &candidate : m_candidates) {
        total += candidate.quantizer.clipped();
    }
    return total;
}

} // namespace rfx
