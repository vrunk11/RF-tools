/******************************************************************************
 * sampleformat.cpp
 * rfx library - the sample formats of a capture file (specification 1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "sampleformat.h"

#include "error.h"

#include <algorithm>
#include <array>
#include <vector>

namespace rfx {

namespace {

// The 10-bit value of .lds is stored with the offset ld-decode removes; the
// sample is on the 16-bit scale of the Duplicator's .s16.
constexpr int32_t kLdsZero = 512;
constexpr int kLdsShift = 6;
constexpr size_t kLdsGroupBytes = 5;
constexpr size_t kLdsGroupSamples = 4;

struct Group
{
    uint64_t bytes;
    uint64_t samples;
};

Group groupOf(SampleFormat fmt)
{
    switch (fmt) {
    case SampleFormat::S16:
    case SampleFormat::U16: return {2, 1};
    case SampleFormat::S8:
    case SampleFormat::U8: return {1, 1};
    case SampleFormat::Lds: break;
    }
    return {kLdsGroupBytes, kLdsGroupSamples};
}

std::array<int32_t, 4> unpackLds(const uint8_t *b)
{
    return {
        (((int32_t(b[0]) << 2) | (int32_t(b[1]) >> 6)) - kLdsZero) * (1 << kLdsShift),
        ((((int32_t(b[1]) & 0x3F) << 4) | (int32_t(b[2]) >> 4)) - kLdsZero) * (1 << kLdsShift),
        ((((int32_t(b[2]) & 0x0F) << 6) | (int32_t(b[3]) >> 2)) - kLdsZero) * (1 << kLdsShift),
        ((((int32_t(b[3]) & 0x03) << 8) | int32_t(b[4])) - kLdsZero) * (1 << kLdsShift),
    };
}

// Whether `v` is a sample an .lds file can hold.
bool ldsFits(int32_t v)
{
    return v % (1 << kLdsShift) == 0 && v / (1 << kLdsShift) >= -kLdsZero && v / (1 << kLdsShift) < kLdsZero;
}

void packLds(const int32_t *v, uint8_t *out)
{
    uint32_t t[4];
    for (int i = 0; i < 4; ++i) {
        t[i] = static_cast<uint32_t>((v[i] >> kLdsShift) + kLdsZero) & 0x3FF;
    }
    out[0] = static_cast<uint8_t>(t[0] >> 2);
    out[1] = static_cast<uint8_t>(((t[0] & 3) << 6) | (t[1] >> 4));
    out[2] = static_cast<uint8_t>(((t[1] & 15) << 4) | (t[2] >> 6));
    out[3] = static_cast<uint8_t>(((t[2] & 63) << 2) | (t[3] >> 8));
    out[4] = static_cast<uint8_t>(t[3] & 255);
}

} // namespace

const char *formatName(SampleFormat fmt)
{
    switch (fmt) {
    case SampleFormat::S16: return "s16";
    case SampleFormat::U16: return "r16";
    case SampleFormat::S8: return "s8";
    case SampleFormat::U8: return "r8";
    case SampleFormat::Lds: break;
    }
    return "lds";
}

bool parseFormat(const std::string &name, SampleFormat &fmt)
{
    if (name == "s16") {
        fmt = SampleFormat::S16;
    } else if (name == "r16" || name == "u16") {
        fmt = SampleFormat::U16;
    } else if (name == "s8") {
        fmt = SampleFormat::S8;
    } else if (name == "r8" || name == "u8") {
        fmt = SampleFormat::U8;
    } else if (name == "lds") {
        fmt = SampleFormat::Lds;
    } else {
        return false;
    }
    return true;
}

uint64_t formatSamples(SampleFormat fmt, uint64_t size)
{
    const Group g = groupOf(fmt);
    return size / g.bytes * g.samples;
}

uint64_t formatCovered(SampleFormat fmt, uint64_t size)
{
    const Group g = groupOf(fmt);
    return formatSamples(fmt, size) / g.samples * g.bytes;
}

void loadSamples(SampleFormat fmt, PosFile &file, uint64_t count, int64_t start, int32_t *out, size_t n)
{
    std::fill(out, out + n, 0);
    const int64_t lo = std::max<int64_t>(start, 0);
    const int64_t hi = std::min<int64_t>(start + static_cast<int64_t>(n), static_cast<int64_t>(count));
    if (hi <= lo) {
        return;
    }
    int32_t *dst = out + (lo - start);
    const size_t take = static_cast<size_t>(hi - lo);
    const Group g = groupOf(fmt);
    const uint64_t g0 = static_cast<uint64_t>(lo) / g.samples;
    const uint64_t g1 = (static_cast<uint64_t>(hi) + g.samples - 1) / g.samples;
    std::vector<uint8_t> buf(static_cast<size_t>((g1 - g0) * g.bytes));
    file.readAt(buf.data(), buf.size(), g0 * g.bytes);
    switch (fmt) {
    case SampleFormat::S16:
        for (size_t i = 0; i < take; ++i) {
            dst[i] = static_cast<int16_t>(static_cast<uint16_t>(buf[2 * i] | (buf[2 * i + 1] << 8)));
        }
        break;
    case SampleFormat::U16:
        for (size_t i = 0; i < take; ++i) {
            dst[i] = static_cast<uint16_t>(buf[2 * i] | (buf[2 * i + 1] << 8));
        }
        break;
    case SampleFormat::S8:
        for (size_t i = 0; i < take; ++i) {
            dst[i] = static_cast<int8_t>(buf[i]);
        }
        break;
    case SampleFormat::U8:
        for (size_t i = 0; i < take; ++i) {
            dst[i] = buf[i];
        }
        break;
    case SampleFormat::Lds: {
        size_t i = 0;
        size_t skip = static_cast<size_t>(static_cast<uint64_t>(lo) - g0 * g.samples);
        for (size_t at = 0; at + kLdsGroupBytes <= buf.size(); at += kLdsGroupBytes) {
            const std::array<int32_t, 4> t = unpackLds(&buf[at]);
            for (size_t s = skip; s < kLdsGroupSamples && i < take; ++s) {
                dst[i++] = t[s];
            }
            skip = 0;
        }
        break;
    }
    }
}

void storeSamples(SampleFormat fmt, PosFile &file, uint64_t start, const int32_t *values, size_t n)
{
    const Group g = groupOf(fmt);
    if (start % g.samples != 0) {
        throw Error("a segment must start on a group of samples");
    }
    std::vector<uint8_t> raw;
    raw.reserve(n * g.bytes / g.samples + kLdsGroupBytes);
    switch (fmt) {
    case SampleFormat::S16:
    case SampleFormat::U16:
        for (size_t i = 0; i < n; ++i) {
            const uint16_t v = static_cast<uint16_t>(values[i]);
            raw.push_back(static_cast<uint8_t>(v & 0xFF));
            raw.push_back(static_cast<uint8_t>(v >> 8));
        }
        break;
    case SampleFormat::S8:
    case SampleFormat::U8:
        for (size_t i = 0; i < n; ++i) {
            raw.push_back(static_cast<uint8_t>(values[i]));
        }
        break;
    case SampleFormat::Lds:
        if (n % kLdsGroupSamples != 0) {
            throw Error("an .lds segment must hold whole groups of four samples");
        }
        for (size_t i = 0; i < n; ++i) {
            if (!ldsFits(values[i])) {
                throw Error("the sample " + std::to_string(values[i]) + " does not fit the 10 bits of an .lds file");
            }
        }
        for (size_t i = 0; i < n; i += kLdsGroupSamples) {
            uint8_t out[kLdsGroupBytes];
            packLds(&values[i], out);
            raw.insert(raw.end(), out, out + kLdsGroupBytes);
        }
        break;
    }
    file.writeAt(raw.data(), raw.size(), start / g.samples * g.bytes);
}

} // namespace rfx
