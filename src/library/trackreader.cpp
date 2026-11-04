/******************************************************************************
 * trackreader.cpp
 * rfx library - reading the samples of the tracks of an .rfx file, or of a raw
 * capture, at any place
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "trackreader.h"

#include "capture.h"
#include "error.h"
#include "ffmpeg.h"
#include "segment.h"

#include <QFileInfo>

#include <algorithm>

namespace rfx {

namespace {

// Segments kept: a window of a few segments over every track, and what it was
// predicted from. About a megabyte a segment at the default segment length.
constexpr size_t kHeldSegments = 160;

// The samples looked at to find the bits a raw capture carries, and read at a time.
constexpr uint64_t kScannedSamples = uint64_t(1) << 24;
constexpr size_t kScanChunk = size_t(1) << 20;
// Samples a segment stands for in the metadata of a raw capture (not used to read it).
constexpr size_t kCaptureSegment = size_t(1) << 18;

constexpr int kEightBits = 8;
constexpr int kStoredBits = 16;

} // namespace

uint32_t shiftForBits(SampleFormat format, int bits)
{
    const int width = (format == SampleFormat::S8 || format == SampleFormat::U8) ? kEightBits : kStoredBits;
    return static_cast<uint32_t>(std::max(0, width - bits));
}

TrackReader::TrackReader(const QString &path)
    : m_path(path)
    , m_fileSize(static_cast<uint64_t>(QFileInfo(path).size()))
    , m_archive(std::make_unique<Archive>(path))
    , m_meta(m_archive->meta())
{
    if (m_meta.sources.empty()) {
        throw Error(path.toStdString() + " holds no track");
    }
    m_windows = std::make_unique<ReferenceWindows>([this](size_t track, int64_t start, int32_t *out, size_t n) { read(track, start, n, out); }, m_meta,
                                                   m_archive->tables());
}

TrackReader::TrackReader(const QString &path, const CaptureSpec &spec)
    : m_path(path)
    , m_fileSize(static_cast<uint64_t>(QFileInfo(path).size()))
    , m_file(std::make_unique<PosFile>(path, PosFile::Mode::Read))
{
    SourceMeta sm;
    sm.name = spec.name.empty() ? QFileInfo(path).fileName().toStdString() : spec.name;
    sm.size = m_file->size();
    m_fileSize = sm.size;
    sm.fmt = formatName(spec.format);
    sm.words = formatSamples(spec.format, sm.size);
    if (spec.compressed) {
        // Its length and the rate of its header, from the header of the FLAC (exactly) or else from what ffmpeg says of it.
        Compressed c;
        if (const std::optional<FlacInfo> info = readFlacInfo(path)) {
            if (info->channels != 1) {
                throw Error(path.toStdString() + " has " + std::to_string(info->channels) + " channels: a capture has one");
            }
            c.headerRate = static_cast<double>(info->sampleRate);
            c.samples = info->samples;
            // A capture written to a pipe holds more than its header says (or the header says none): the last frames know.
            if (const std::optional<uint64_t> real = flacSamplesFromTail(path, *info)) {
                c.samples = *real;
            }
        }
        if (c.headerRate <= 0.0 || c.samples == 0) {
            const std::optional<CompressedHeader> header = probeCompressed(path);
            if (!header || header->headerRate <= 0 || header->durationSeconds <= 0.0) {
                throw Error("ffmpeg cannot read the length of " + path.toStdString());
            }
            c.headerRate = static_cast<double>(header->headerRate);
            c.samples = static_cast<uint64_t>(std::llround(header->durationSeconds * c.headerRate));
        }
        sm.fmt = formatName(SampleFormat::S16);
        sm.words = c.samples;
        m_compressed = c;
    }
    sm.rateHz = spec.rateHz;

    sm.sidecars = findSidecars(path);
    if (sm.words == 0) {
        throw Error(path.toStdString() + " holds no sample in the format " + sm.fmt);
    }
    if (spec.bits) {
        sm.shift = shiftForBits(spec.format, *spec.bits);
    } else {
        // The low bits that are zero in every sample of the first stretch (specification 1).
        uint32_t any = 0;
        std::vector<int32_t> chunk(kScanChunk);
        const uint64_t scanned = std::min<uint64_t>(sm.words, m_compressed ? kScanChunk : kScannedSamples);
        for (uint64_t at = 0; at < scanned; at += kScanChunk) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(kScanChunk, scanned - at));
            if (m_compressed) {
                readCompressed(static_cast<int64_t>(at), n, chunk.data(), false);
            } else {
                loadSamples(spec.format, *m_file, sm.words, static_cast<int64_t>(at), chunk.data(), n);
            }
            for (size_t i = 0; i < n; ++i) {
                any |= static_cast<uint32_t>(chunk[i]);
            }
        }
        while (any != 0 && (any & 1U) == 0) {
            any >>= 1;
            ++sm.shift;
        }
    }
    m_meta.segment = kCaptureSegment;
    m_meta.sources.push_back(std::move(sm));
}

const LagTable *TrackReader::lagTable(size_t k, size_t j) const
{
    if (!m_archive) {
        return nullptr;
    }
    const auto it = m_archive->tables().find({k, j});
    return it == m_archive->tables().end() ? nullptr : &it->second;
}

TrackReader::Segment TrackReader::segment(size_t k, uint64_t g)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_held.find({k, g});
        if (it != m_held.end()) {
            it->second.tick = ++m_tick;
            return it->second.samples;
        }
    }

    const SourceMeta &sm = m_meta.sources[k];
    const uint64_t s = m_meta.segment;
    const uint64_t s0 = g * s;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(s, sm.words - s0));
    const auto block = m_archive->blocks(k).find(g);
    if (block == m_archive->blocks(k).end()) {
        throw Error(sm.name + ": segment " + std::to_string(g) + " is missing");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(block->second.length));
    if (!bytes.empty()) {
        m_archive->file().readAt(bytes.data(), bytes.size(), block->second.offset);
    }
    const bool useRef = !sm.refs.empty();
    std::vector<float> window;
    if (useRef) {
        window = m_windows->window(k, static_cast<int64_t>(s0), n);
    }
    std::vector<int32_t> residuals;
    if (!decodeResiduals(bytes.data(), bytes.size(), n, residuals)) {
        throw Error(sm.name + ": segment " + std::to_string(g) + " is corrupt");
    }
    std::vector<int32_t> y = reconstruct(residuals, sm.preset(), useRef, window.data(), n + 2 * sm.taps);
    for (int32_t &v : y) {
        v = static_cast<int32_t>((static_cast<uint32_t>(v) + static_cast<uint32_t>(sm.mean)) << sm.shift);
    }
    const Segment decoded = std::make_shared<const std::vector<int32_t>>(std::move(y));

    std::lock_guard<std::mutex> lock(m_mutex);
    m_held[{k, g}] = {decoded, ++m_tick};
    while (m_held.size() > kHeldSegments) {
        auto oldest = m_held.begin();
        for (auto it = m_held.begin(); it != m_held.end(); ++it) {
            if (it->second.tick < oldest->second.tick) {
                oldest = it;
            }
        }
        m_held.erase(oldest);
    }
    return decoded;
}

void TrackReader::read(size_t k, int64_t start, size_t n, int32_t *out)
{
    const SourceMeta &sm = m_meta.sources[k];
    if (m_compressed) {
        readCompressed(start, n, out, true);
        return;
    }
    if (!m_archive) {
        // A raw capture is read where it is.
        loadSamples(sm.format(), *m_file, sm.words, start, out, n);
        return;
    }
    std::fill(out, out + n, 0);
    const int64_t words = static_cast<int64_t>(sm.words);
    const int64_t from = std::max<int64_t>(start, 0);
    const int64_t to = std::min<int64_t>(start + static_cast<int64_t>(n), words);
    if (to <= from) {
        return;
    }
    const int64_t s = static_cast<int64_t>(m_meta.segment);
    for (int64_t g = from / s; g * s < to; ++g) {
        const Segment seg = segment(k, static_cast<uint64_t>(g));
        const int64_t lo = std::max<int64_t>(from, g * s);
        const int64_t hi = std::min<int64_t>(to, g * s + static_cast<int64_t>(seg->size()));
        for (int64_t p = lo; p < hi; ++p) {
            out[p - start] = (*seg)[static_cast<size_t>(p - g * s)];
        }
    }
}

void TrackReader::readOnce(size_t k, int64_t start, size_t n, int32_t *out)
{
    if (m_compressed) {
        readCompressed(start, n, out, false);
        return;
    }
    read(k, start, n, out);
}

// The samples of a compressed capture, from the stretches that are held, and from ffmpeg for those that are not.
void TrackReader::readCompressed(int64_t start, size_t n, int32_t *out, bool keep)
{
    constexpr uint64_t kStretch = uint64_t(1) << 18;
    // What a run of missing stretches may be, in one call of ffmpeg.
    constexpr uint64_t kMostPerCall = 8;
    // About 100 MB of decoded samples.
    constexpr size_t kMostHeld = 200;

    std::fill(out, out + n, 0);
    const int64_t total = static_cast<int64_t>(m_compressed->samples);
    const int64_t from = std::max<int64_t>(start, 0);
    const int64_t to = std::min<int64_t>(start + static_cast<int64_t>(n), total);
    if (to <= from) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t first = static_cast<uint64_t>(from) / kStretch;
    const uint64_t last = static_cast<uint64_t>(to - 1) / kStretch;

    if (!keep) {
        // Just the range, decoded for this question alone, unless what is held covers it.
        bool covered = true;
        for (uint64_t c = first; c <= last && covered; ++c) {
            covered = m_stretches.count(c) != 0;
        }
        if (!covered) {
            const std::vector<int16_t> own = decodeWindow(m_path, m_compressed->headerRate, static_cast<uint64_t>(from), static_cast<size_t>(to - from));
            for (size_t i = 0; i < own.size(); ++i) {
                out[from - start + static_cast<int64_t>(i)] = own[i];
            }
            return;
        }
    }

    // Decode the stretches that are missing, a run of them in one call.
    for (uint64_t c = first; c <= last;) {
        if (m_stretches.count(c) != 0) {
            ++c;
            continue;
        }
        uint64_t end = c;
        while (end <= last && end - c < kMostPerCall && m_stretches.count(end) == 0) {
            ++end;
        }
        const uint64_t samples = std::min<uint64_t>((end - c) * kStretch, m_compressed->samples - c * kStretch);
        const std::vector<int16_t> run = decodeWindow(m_path, m_compressed->headerRate, c * kStretch, static_cast<size_t>(samples));
        for (uint64_t s = c; s < end; ++s) {
            const size_t lo = static_cast<size_t>((s - c) * kStretch);
            if (lo >= run.size()) {
                // (the file holds less than it said: what is not there is zero)
                m_stretches[s] = Stretch{std::make_shared<const std::vector<int16_t>>(), ++m_tick};
                continue;
            }
            const size_t hi = std::min<size_t>(run.size(), lo + static_cast<size_t>(kStretch));
            m_stretches[s] = Stretch{std::make_shared<const std::vector<int16_t>>(run.begin() + static_cast<std::ptrdiff_t>(lo), run.begin() + static_cast<std::ptrdiff_t>(hi)), ++m_tick};
        }
        c = end;
    }
    for (uint64_t c = first; c <= last; ++c) {
        Stretch &st = m_stretches[c];
        st.tick = ++m_tick;
        const int64_t base = static_cast<int64_t>(c * kStretch);
        const int64_t lo = std::max<int64_t>(from, base);
        const int64_t hi = std::min<int64_t>(to, base + static_cast<int64_t>(st.samples->size()));
        for (int64_t p = lo; p < hi; ++p) {
            out[p - start] = (*st.samples)[static_cast<size_t>(p - base)];
        }
    }
    // The least recently used go when there are too many.
    while (m_stretches.size() > kMostHeld) {
        auto oldest = m_stretches.begin();
        for (auto it = m_stretches.begin(); it != m_stretches.end(); ++it) {
            if (it->second.tick < oldest->second.tick) {
                oldest = it;
            }
        }
        m_stretches.erase(oldest);
    }
}

} // namespace rfx
