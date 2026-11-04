/******************************************************************************
 * analysis.h
 * rfx library - looking at a stretch of samples: its spectrum, what it says of
 * the signal and its noise, and how a track drifts against its reference
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "lagtable.h"
#include "sampleformat.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rfx {

// A stretch of samples in the units of the converter: the 16-bit value divided by
// 2^(16 - bits), so that a 10-bit capture goes from -512 to 511.
double toConverterUnits(SampleFormat fmt, int32_t canonical, int bits);

// The bits a track carries, from its format and its `shift` (specification 1).
int carriedBits(SampleFormat fmt, uint32_t shift);

// The spectrum of a stretch, averaged over Hann-windowed frames of `fftSize`
// samples that overlap by half (at most kMostFrames of them, spread over the
// stretch; a stretch shorter than a frame is padded with zeros). The power of a bin
// is scaled so that white noise of variance v gives v in every bin.
constexpr size_t kMostFrames = 64;

struct Spectrum
{
    size_t fftSize = 0;
    size_t frames = 0;
    double binHz = 0.0;
    // |X|^2 over the window's power, bins 0 .. fftSize / 2, in squared units: for
    // noise, which gives its variance in every bin.
    std::vector<double> power;
    // The same bins in dB relative to a sine that fills the range `fullScale` (0
    // dB for such a sine, whatever the window), for drawing.
    std::vector<float> db;
};

// `fullScale` is the largest value of a sample (512 for ten bits). Throws
// rfx::Error unless `fftSize` is a power of two, at least 8.
Spectrum analyseSpectrum(const std::vector<double> &samples, size_t fftSize, double rateHz, double fullScale);

// What a stretch says of the signal in a band.
struct SampleStats
{
    // The mean of |x - mean|, in converter units.
    double meanAmplitude = 0.0;
    // The mean power of the band over the noise floor, in dB.
    double snrDb = 0.0;
    // The RMS of white noise whose power is the band's floor (the median of its
    // bins, corrected for the frames averaged), in converter units.
    double noiseFloor = 0.0;
    // (SNR - 1.76) / 6.02: the bits that carry something above the noise.
    double effectiveBits = 0.0;
};

// `bandHighHz` is the top of the band that counts, from DC (14 MHz for a
// LaserDisc, or less when the rate cannot hold it: 1.5 MHz short of the Nyquist
// limit).
SampleStats computeStats(const std::vector<double> &samples, const Spectrum &spectrum, double bandHighHz);

double defaultBandHighHz(double rateHz);

// The speed of a track against its reference, in parts per million, at `points`
// places evenly spread over [0, length): the slope of the lag over `window` samples
// around each. Zero is the speed the capture was made at, with no correction. The
// lag is the position in the reference of a position of the track, so a lag that
// grows means more of the reference passes under each sample of the track: positive
// is faster, negative is slower. The window is cut at the ends of the track.
std::vector<double> speedDeviationPpm(const LagTable &table, uint64_t length, size_t points, uint64_t window);

// The lag at `points` places evenly spread over [0, length), for drawing.
std::vector<int64_t> lagSeries(const LagTable &table, uint64_t length, size_t points);

} // namespace rfx
