/******************************************************************************
 * requantize.cpp
 * rfx library - requantising samples in a stream or a file of any capture format
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools. See requantize.h.
 ******************************************************************************/

#include "requantize.h"

#include "convert.h"
#include "error.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace rfx {

namespace {

constexpr double kMegahertz = 1.0e6;
constexpr size_t kBytesPerSample = 2;
// Bits of a stored sample.
constexpr int kStoredSampleBits = 16;
// The low bits a sample of a narrow format has as a 16-bit one.
constexpr int kEightBitScale = 8;
// Samples read from a file at a time while a segment is gathered.
constexpr size_t kReadChunk = size_t{1} << 18;

} // namespace

int bitsFoundIn(const std::vector<int16_t> &samples)
{
    uint16_t any = 0;
    for (const int16_t sample : samples) {
        any = static_cast<uint16_t>(any | static_cast<uint16_t>(sample));
    }
    if (any == 0) {
        return kConverterBits;
    }
    int zeros = 0;
    while ((any & 1U) == 0) {
        any = static_cast<uint16_t>(any >> 1);
        ++zeros;
    }
    return kStoredSampleBits - zeros;
}

int bitsOfFormat(SampleFormat fmt, uint32_t shift)
{
    // A zero track has every bit zero: the converter's width stands for it.
    const uint32_t narrow = (fmt == SampleFormat::S8 || fmt == SampleFormat::U8) ? kEightBitScale : 0;
    const uint32_t zeros = std::min<uint32_t>(shift + narrow, kStoredSampleBits);
    if (zeros >= kStoredSampleBits) {
        return kConverterBits;
    }
    return kStoredSampleBits - static_cast<int>(zeros);
}

const char *requantizeLogHeader()
{
    return "track,segment,start_seconds,samples,bits_kept,bits_dropped,shaped,rise_db,noise_floor_lsb,carrier_to_noise_db,"
           "held_back_low_mhz,held_back_high_mhz,held_back_floor_lsb\n";
}

std::vector<std::pair<std::string, std::string>> requantizationTags(const RequantizeOptions &options, const RequantizerSettings &settings)
{
    std::vector<std::pair<std::string, std::string>> tags;
    if (!options.requantize) {
        tags.emplace_back("DDD_REQUANTIZATION", "off");
        return tags;
    }
    const int level = settings.marginLevel;
    tags.emplace_back("DDD_REQUANTIZATION", "dynamic");
    tags.emplace_back("DDD_REQUANTIZATION_MARGIN",
                      std::to_string(level) + " (" + marginLevelName(level) + ", " + formatDecimal(marginLimitDb(level), 2) + " dB)");
    if (!settings.protectedBands.empty()) {
        tags.emplace_back("DDD_REQUANTIZATION_BANDS", describeBands(settings.protectedBands));
    }
    tags.emplace_back("DDD_REQUANTIZATION_SHAPING",
                      std::string(settings.shaping == RequantizerSettings::Shaping::Adaptive ? "adaptive" : "fixed") + ", order "
                          + std::to_string(settings.shapingOrder) + ", " + formatDecimal(settings.shapingDepthDb, 1) + " dB"
                          + (settings.shapingZones.empty() ? std::string() : "; zones " + describeShapingZones(settings.shapingZones)));
    return tags;
}

RequantizeResult requantizeStream(const RequantizeOptions &options, const StreamSpec &spec, const SegmentReader &read,
                                  const SegmentWriter &write)
{
    RequantizeResult result;
    result.name = spec.name;
    result.rateMhz = spec.rateMhz;
    result.requantized = options.requantize;

    std::vector<int16_t> segment(RfRequantizer::kSegmentSamples);
    segment.resize(read(segment.data(), segment.size()));
    if (segment.empty()) {
        throw Error(spec.name + " holds no samples");
    }

    // The bits it carries: as given, as the capture says, or from the samples
    // themselves.
    int inputBits = 0;
    if (options.inputBits) {
        inputBits = *options.inputBits;
    } else if (spec.knownBits) {
        inputBits = *spec.knownBits;
    } else {
        inputBits = bitsFoundIn(segment);
    }
    inputBits = std::clamp(inputBits, kFewestInputBits, kMostInputBits);
    result.inputBits = inputBits;

    RequantizerRequest request;
    request.sampleRateMhz = spec.rateMhz;
    request.marginLevel = options.marginLevel;
    request.bands = options.bands;
    request.inputBits = inputBits;
    request.adaptive = options.adaptive;
    request.depthsDb = options.depthsDb;
    request.order = options.order;
    if (options.requantize) {
        const std::string problem = shapingDepthsProblem(request);
        if (!problem.empty()) {
            throw UsageError("--depth: " + problem);
        }
    }
    std::unique_ptr<RfRequantizer> requantizer;
    if (options.requantize) {
        requantizer = std::make_unique<RfRequantizer>(settingsFor(request));
    }

    // As a capture does it: segment after segment from the start, the last
    // whatever is left, each decided and, where it is being kept, requantised in
    // place before it is written.
    RequantizationStatus status;
    uint64_t position = 0;
    uint64_t segments = 0;
    int lastPercent = -1;
    const double samplesPerSecond = spec.rateMhz * kMegahertz;
    const auto started = std::chrono::steady_clock::now();
    while (!segment.empty()) {
        const RequantizerDecision decision = requantizer ? requantizer->process(segment.data(), segment.size(), static_cast<bool>(write))
                                                         : RequantizerDecision();
        const double noiseFloor = requantizer ? requantizer->noiseFloorLsb() : 0.0;
        const double carrierToNoise = requantizer ? requantizer->carrierToNoiseDb() : 0.0;
        status.record(decision, position, segment.size(), requantizer ? requantizer->clipped() : 0);

        if (spec.log) {
            const std::string line = std::to_string(spec.logTrack) + "," + std::to_string(segments) + ","
                + formatDecimal(static_cast<double>(position) / samplesPerSecond, 4) + "," + std::to_string(segment.size()) + ","
                + std::to_string(inputBits - std::max(decision.lsbDrop, 0)) + "," + std::to_string(decision.lsbDrop) + ","
                + (decision.shaped ? "1" : "0") + "," + formatDecimal(decision.degradationDb, 3) + "," + formatDecimal(noiseFloor, 3) + ","
                + formatDecimal(carrierToNoise, 2) + "," + formatDecimal(decision.limitLowMhz, 2) + "," + formatDecimal(decision.limitHighMhz, 2)
                + "," + formatDecimal(decision.limitFloorLsb, 3) + "\n";
            spec.log(line);
        }

        if (write) {
            write(segment.data(), segment.size());
        }
        position += segment.size();
        ++segments;

        // A segment at a time, as a capture's own pipe hands them on: no sooner than
        // the moment the last of its samples would have been taken.
        if (options.realtime) {
            std::this_thread::sleep_until(started
                                          + std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::duration<double>(static_cast<double>(position) / samplesPerSecond)));
        }
        if (spec.progress && spec.total > 0) {
            const int percent = static_cast<int>((position * 100) / spec.total);
            if (percent != lastPercent) {
                lastPercent = percent;
                spec.progress(percent);
            }
        }

        segment.resize(RfRequantizer::kSegmentSamples);
        segment.resize(read(segment.data(), segment.size()));
    }

    result.summary = status.summary();
    result.tookSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.lastsSeconds = static_cast<double>(position) / samplesPerSecond;
    return result;
}

SegmentReader fileReader(PosFile &in, SampleFormat fmt, uint64_t words, const std::string &name)
{
    PosFile *const file = &in;
    uint64_t at = 0;
    std::vector<int32_t> wide;
    return [file, fmt, words, name, at, wide](int16_t *dst, size_t count) mutable -> size_t {
        size_t done = 0;
        while (done < count && at < words) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(std::min<uint64_t>(words - at, count - done), kReadChunk));
            wide.resize(n);
            loadSamples(fmt, *file, words, static_cast<int64_t>(at), wide.data(), n);
            for (size_t i = 0; i < n; ++i) {
                if (!toI16(fmt, wide[i], dst[done + i])) {
                    throw Error(name + ": a sample does not fit 16 bits");
                }
            }
            at += n;
            done += n;
        }
        return done;
    };
}

SegmentWriter fileWriter(PosFile &out, SampleFormat fmt, const std::string &name)
{
    PosFile *const file = &out;
    uint64_t at = 0;
    std::vector<int32_t> wide;
    return [file, fmt, name, at, wide](const int16_t *samples, size_t count) mutable {
        wide.resize(count);
        for (size_t i = 0; i < count; ++i) {
            wide[i] = fromI16(fmt, samples[i]);
        }
        try {
            storeSamples(fmt, *file, at, wide.data(), count);
        } catch (const Error &e) {
            throw Error(name + ": " + e.what() + " (the format " + formatName(fmt) + " cannot hold the requantised samples)");
        }
        at += count;
    };
}

SegmentReader standardReader(std::FILE *in)
{
    std::vector<uint8_t> bytes;
    return [in, bytes](int16_t *dst, size_t count) mutable -> size_t {
        bytes.resize(count * kBytesPerSample);
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), in) / kBytesPerSample;
        for (size_t i = 0; i < got; ++i) {
            dst[i] = static_cast<int16_t>(static_cast<uint16_t>(bytes[i * kBytesPerSample] | (bytes[(i * kBytesPerSample) + 1] << 8)));
        }
        return got;
    };
}

SegmentWriter standardWriter(std::FILE *out)
{
    std::vector<uint8_t> bytes;
    return [out, bytes](const int16_t *samples, size_t count) mutable {
        bytes.resize(count * kBytesPerSample);
        for (size_t i = 0; i < count; ++i) {
            const auto word = static_cast<uint16_t>(samples[i]);
            bytes[i * kBytesPerSample] = static_cast<uint8_t>(word & 0xFFU);
            bytes[(i * kBytesPerSample) + 1] = static_cast<uint8_t>(word >> 8);
        }
        if (std::fwrite(bytes.data(), 1, bytes.size(), out) != bytes.size()) {
            throw Error("Standard output stopped taking samples");
        }
    };
}

void copyTail(PosFile &in, PosFile &out, SampleFormat fmt)
{
    const uint64_t size = in.size();
    const uint64_t covered = formatCovered(fmt, size);
    if (size > covered) {
        std::vector<uint8_t> tail(static_cast<size_t>(size - covered));
        in.readAt(tail.data(), tail.size(), covered);
        out.writeAt(tail.data(), tail.size(), covered);
    }
}

RequantizeResult requantizeFile(const RequantizeOptions &options, const StreamSpec &spec, PosFile &in, SampleFormat fmt, PosFile *out,
                                SampleFormat outFormat)
{
    SegmentWriter writer;
    if (out) {
        writer = fileWriter(*out, outFormat, spec.name);
    }
    RequantizeResult result = requantizeStream(options, spec, fileReader(in, fmt, spec.total, spec.name), writer);
    if (out && outFormat == fmt) {
        copyTail(in, *out, fmt);
    }
    return result;
}

} // namespace rfx
