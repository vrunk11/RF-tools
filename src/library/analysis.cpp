/******************************************************************************
 * analysis.cpp
 * rfx library - looking at a stretch of samples
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "analysis.h"

#include "convert.h"
#include "error.h"

#include <algorithm>
#include <cmath>

namespace rfx {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kStoredBits = 16;
constexpr int kEightBitExtra = 8;
constexpr double kLaserDiscBandHz = 14.0e6;
constexpr double kNyquistMarginHz = 1.5e6;
constexpr double kSnrOffsetDb = 1.76;
constexpr double kDbPerBit = 6.02;

// An in-place radix-2 transform of `real` + i `imag` (size a power of two).
void transform(std::vector<double> &real, std::vector<double> &imag, const std::vector<double> &cosines, const std::vector<double> &sines)
{
    const size_t size = real.size();
    for (size_t i = 1, j = 0; i < size; ++i) {
        size_t bit = size >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }
    for (size_t length = 2; length <= size; length <<= 1) {
        const size_t half = length / 2;
        const size_t stride = size / length;
        for (size_t start = 0; start < size; start += length) {
            for (size_t j = 0; j < half; ++j) {
                const double wr = cosines[j * stride];
                const double wi = -sines[j * stride];
                const size_t upper = start + j + half;
                const double pr = real[upper] * wr - imag[upper] * wi;
                const double pi = real[upper] * wi + imag[upper] * wr;
                real[upper] = real[start + j] - pr;
                imag[upper] = imag[start + j] - pi;
                real[start + j] += pr;
                imag[start + j] += pi;
            }
        }
    }
}

// The median of a chi-squared variable of k degrees of freedom over k, which is
// what dividing the median of a bin's averaged power by turns it into the mean
// (Wilson-Hilferty).
double medianOverMean(double degrees)
{
    const double t = 1.0 - 2.0 / (9.0 * degrees);
    return t * t * t;
}

} // namespace

double toConverterUnits(SampleFormat fmt, int32_t canonical, int bits)
{
    int16_t wide = 0;
    if (!toI16(fmt, canonical, wide)) {
        wide = canonical < 0 ? INT16_MIN : INT16_MAX;
    }
    return static_cast<double>(wide) / std::ldexp(1.0, kStoredBits - bits);
}

int carriedBits(SampleFormat fmt, uint32_t shift)
{
    const uint32_t narrow = (fmt == SampleFormat::S8 || fmt == SampleFormat::U8) ? kEightBitExtra : 0;
    const uint32_t zeros = std::min<uint32_t>(shift + narrow, kStoredBits);
    return zeros >= kStoredBits ? 10 : kStoredBits - static_cast<int>(zeros);
}

Spectrum analyseSpectrum(const std::vector<double> &samples, size_t fftSize, double rateHz, double fullScale)
{
    if (fftSize < 8 || (fftSize & (fftSize - 1)) != 0) {
        throw Error("the FFT size must be a power of two");
    }
    Spectrum out;
    out.fftSize = fftSize;
    out.binHz = rateHz / static_cast<double>(fftSize);
    const size_t bins = fftSize / 2 + 1;
    out.power.assign(bins, 0.0);
    out.db.assign(bins, -200.0F);
    if (samples.empty()) {
        return out;
    }

    std::vector<double> window(fftSize), cosines(fftSize / 2), sines(fftSize / 2);
    double windowPower = 0.0;
    double windowSum = 0.0;
    for (size_t i = 0; i < fftSize; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(fftSize));
        windowPower += window[i] * window[i];
        windowSum += window[i];
    }
    std::vector<double> raw(bins, 0.0);
    for (size_t i = 0; i < fftSize / 2; ++i) {
        cosines[i] = std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(fftSize));
        sines[i] = std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(fftSize));
    }

    // Frames half a frame apart over the stretch, spread out when there are more
    // than can be afforded.
    const size_t step = fftSize / 2;
    size_t count = samples.size() >= fftSize ? (samples.size() - fftSize) / step + 1 : 1;
    size_t every = 1;
    if (count > kMostFrames) {
        every = (count + kMostFrames - 1) / kMostFrames;
    }
    std::vector<double> real(fftSize), imag(fftSize);
    for (size_t f = 0; f < count; f += every) {
        const size_t at = f * step;
        double mean = 0.0;
        const size_t have = std::min(fftSize, samples.size() - std::min(at, samples.size()));
        for (size_t i = 0; i < have; ++i) {
            mean += samples[at + i];
        }
        mean = have ? mean / static_cast<double>(have) : 0.0;
        for (size_t i = 0; i < fftSize; ++i) {
            real[i] = i < have ? (samples[at + i] - mean) * window[i] : 0.0;
            imag[i] = 0.0;
        }
        transform(real, imag, cosines, sines);
        for (size_t b = 0; b < bins; ++b) {
            raw[b] += real[b] * real[b] + imag[b] * imag[b];
        }
        ++out.frames;
    }
    // Two scalings of the same sums: `power` for noise (white noise of variance v
    // gives v in every bin), and the decibels for a tone (a sine that fills the
    // range is 0 dB, whatever the window).
    const double reference = fullScale * fullScale / 4.0;
    for (size_t b = 0; b < bins; ++b) {
        raw[b] /= static_cast<double>(out.frames);
        out.power[b] = raw[b] / windowPower;
        out.db[b] = static_cast<float>(10.0 * std::log10(std::max(raw[b] / (windowSum * windowSum), 1e-30) / reference));
    }
    return out;
}

double defaultBandHighHz(double rateHz)
{
    return std::min(kLaserDiscBandHz, rateHz / 2.0 - kNyquistMarginHz);
}

SampleStats computeStats(const std::vector<double> &samples, const Spectrum &spectrum, double bandHighHz)
{
    SampleStats s;
    if (samples.empty() || spectrum.power.empty()) {
        return s;
    }
    double mean = 0.0;
    for (const double v : samples) {
        mean += v;
    }
    mean /= static_cast<double>(samples.size());
    double sum = 0.0;
    for (const double v : samples) {
        sum += std::fabs(v - mean);
    }
    s.meanAmplitude = sum / static_cast<double>(samples.size());

    // The band's bins, from the first above DC.
    std::vector<double> inBand;
    double total = 0.0;
    for (size_t b = 1; b < spectrum.power.size(); ++b) {
        if (static_cast<double>(b) * spectrum.binHz <= bandHighHz) {
            inBand.push_back(spectrum.power[b]);
            total += spectrum.power[b];
        }
    }
    if (inBand.empty()) {
        return s;
    }
    std::nth_element(inBand.begin(), inBand.begin() + static_cast<std::ptrdiff_t>(inBand.size() / 2), inBand.end());
    const double median = inBand[inBand.size() / 2] / medianOverMean(2.0 * static_cast<double>(std::max<size_t>(spectrum.frames, 1)));
    const double floor = std::max(median, 1e-30);
    s.noiseFloor = std::sqrt(floor);
    s.snrDb = 10.0 * std::log10((total / static_cast<double>(inBand.size())) / floor);
    s.effectiveBits = (s.snrDb - kSnrOffsetDb) / kDbPerBit;
    return s;
}

std::vector<double> speedDeviationPpm(const LagTable &table, uint64_t length, size_t points, uint64_t window)
{
    std::vector<double> out(points, 0.0);
    if (length < 2 || points == 0) {
        return out;
    }
    const int64_t last = static_cast<int64_t>(length) - 1;
    const int64_t half = static_cast<int64_t>(std::max<uint64_t>(window, 2) / 2);
    for (size_t i = 0; i < points; ++i) {
        const double at = points > 1 ? static_cast<double>(i) / static_cast<double>(points - 1) : 0.0;
        const int64_t centre = static_cast<int64_t>(at * static_cast<double>(last));
        const int64_t from = std::max<int64_t>(centre - half, 0);
        const int64_t to = std::min<int64_t>(centre + half, last);
        if (to > from) {
            out[i] = static_cast<double>(table.lagAt(to) - table.lagAt(from)) / static_cast<double>(to - from) * 1e6;
        }
    }
    return out;
}

std::vector<int64_t> lagSeries(const LagTable &table, uint64_t length, size_t points)
{
    std::vector<int64_t> out(points, 0);
    for (size_t i = 0; i < points; ++i) {
        const double at = points > 1 ? static_cast<double>(i) / static_cast<double>(points - 1) : 0.0;
        out[i] = table.lagAt(static_cast<int64_t>(at * static_cast<double>(length > 0 ? length - 1 : 0)));
    }
    return out;
}

} // namespace rfx
