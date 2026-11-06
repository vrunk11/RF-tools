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
#include "convert.h"
#include "ebml.h"
#include "error.h"
#include "ffmpeg.h"
#include "flacmode.h"
#include "parallel.h"
#include "segment.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rfx {

namespace {

// Segments kept: a window of a few segments over every track, and what it was
// predicted from. About a megabyte a segment at the default segment length.
constexpr size_t kHeldSegments = 384;

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
    , m_stored(isFlacArchive(path))
{
    if (m_stored) {
        // The FLAC or raw variant: the metadata of its tracks are in rfx.json, and there is nothing to decode but the audio.
        m_meta.segment = kCaptureSegment;
        m_meta.sources = readFlacMeta(path, nullptr, &m_meta.medium);
        m_file = std::make_unique<PosFile>(path, PosFile::Mode::Read);
        indexStored();
    } else {
        m_archive = std::make_unique<Archive>(path);
        m_meta = m_archive->meta();
    }
    if (m_meta.sources.empty()) {
        throw Error(path.toStdString() + " holds no track");
    }
    if (m_stored) {
        return;
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
                readStretches(0, static_cast<int64_t>(at), n, chunk.data(), false);
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

const SavedPass0 *TrackReader::pass0(size_t k) const
{
    if (!m_archive) {
        return nullptr;
    }
    const auto it = m_archive->pass0().find(k);
    return it == m_archive->pass0().end() ? nullptr : &it->second;
}

TrackReader::Segment TrackReader::segment(size_t k, uint64_t g)
{
    std::promise<Segment> promise;
    std::shared_future<Segment> waiting;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_held.find({k, g});
        if (it != m_held.end()) {
            it->second.tick = ++m_tick;
            return it->second.samples;
        }
        const auto flight = m_inflight.find({k, g});
        if (flight != m_inflight.end()) {
            waiting = flight->second;
        } else {
            m_inflight.emplace(std::make_pair(k, g), promise.get_future().share());
        }
    }
    if (waiting.valid()) {
        return waiting.get();
    }
    // (a segment only waits for tracks before its own, so that no two threads wait for each other)
    try {
        const Segment decoded = decodeSegment(k, g);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_inflight.erase({k, g});
        }
        promise.set_value(decoded);
        return decoded;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_inflight.erase({k, g});
        }
        promise.set_exception(std::current_exception());
        throw;
    }
}

TrackReader::Segment TrackReader::decodeSegment(size_t k, uint64_t g)
{

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
    if (m_stored) {
        readStored(k, start, n, out, true);
        return;
    }
    if (m_compressed) {
        readStretches(0, start, n, out, true);
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
    // A long stretch asked for at once (the location of the lines reads megabytes): the segments that are not held are independent
    // of one another, so they are decoded on all the processors. A segment decoded inside this loop reads its references on its
    // own thread, one after the other.
    thread_local bool inPrefetch = false;
    if (!inPrefetch) {
        std::vector<uint64_t> missing;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (int64_t g = from / s; g * s < to; ++g) {
                if (m_held.find({k, static_cast<uint64_t>(g)}) == m_held.end()) {
                    missing.push_back(static_cast<uint64_t>(g));
                }
            }
        }
        if (missing.size() > 1) {
            parallelFor(missing.size(), defaultThreads(), [&](size_t i) {
                inPrefetch = true;
                try {
                    segment(k, missing[i]);
                } catch (...) {
                    inPrefetch = false;
                    throw;
                }
                inPrefetch = false;
            });
        }
    }
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
    if (m_stored) {
        readStored(k, start, n, out, false);
        return;
    }
    if (m_compressed) {
        readStretches(0, start, n, out, false);
        return;
    }
    read(k, start, n, out);
}

// Where the samples of each track of a FLAC or raw variant are: the blocks of every track, in order, each holding one FLAC frame
// (which holds a fixed number of samples) or some PCM samples (specification 9).
void TrackReader::indexStored()
{
    const ebml::Index idx = ebml::scan(*m_file, std::nullopt);
    // The audio tracks, in the order of their numbers, are the tracks of the archive.
    std::vector<std::pair<uint64_t, std::string>> codecs = idx.codecs;
    std::sort(codecs.begin(), codecs.end());
    if (codecs.size() != m_meta.sources.size()) {
        throw Error(m_path.toStdString() + ": " + std::to_string(codecs.size()) + " audio tracks, " + std::to_string(m_meta.sources.size()) + " in rfx.json");
    }
    m_storedTracks.assign(codecs.size(), StoredTrack());
    for (size_t k = 0; k < codecs.size(); ++k) {
        StoredTrack &t = m_storedTracks[k];
        t.pcm = codecs[k].second == "A_PCM/INT/LIT";
        if (!t.pcm && codecs[k].second != "A_FLAC") {
            throw Error(m_path.toStdString() + ": track " + std::to_string(k) + " is " + codecs[k].second + ", not FLAC or PCM");
        }
        if (!t.pcm) {
            for (const auto &p : idx.codecPrivate) {
                if (p.first == codecs[k].first) {
                    t.header = p.second;
                }
            }
            // "fLaC" and the STREAMINFO block, which is the last one: the minimum and maximum block sizes are the 16 bits after its header.
            if (t.header.size() < 4 || std::memcmp(t.header.data(), "fLaC", 4) != 0) {
                t.header.insert(t.header.begin(), {'f', 'L', 'a', 'C'});
            }
            if (t.header.size() < 4 + 4 + 34) {
                throw Error(m_path.toStdString() + ": the FLAC stream header of track " + std::to_string(k) + " is damaged");
            }
            t.header[4] |= 0x80;
            const uint64_t minBlock = (uint64_t(t.header[8]) << 8) | t.header[9];
            const uint64_t maxBlock = (uint64_t(t.header[10]) << 8) | t.header[11];
            if (minBlock != maxBlock || minBlock == 0) {
                throw Error(m_path.toStdString() + ": the frames of track " + std::to_string(k) + " do not have a fixed size");
            }
            t.frameSamples = minBlock;
        }
        uint64_t samples = 0;
        for (const ebml::BlockRef &b : idx.blocks) {
            if (b.track != codecs[k].first) {
                continue;
            }
            t.first.push_back(samples);
            t.offset.push_back(b.off);
            t.length.push_back(static_cast<uint32_t>(b.len));
            samples += t.pcm ? b.len / 2 : t.frameSamples;
        }
    }
}

// `count` samples of a track from sample `from`, as the 16-bit values the stream holds, exactly: PCM is read where it is; FLAC frames
// are put one after the other behind the stream header in a file of their own and decoded by ffmpeg, starting at a frame, so that
// no seek is needed. Fewer come back past the end.
std::vector<int16_t> TrackReader::storedWindow(size_t k, uint64_t from, size_t count)
{
    const StoredTrack &t = m_storedTracks[k];
    const uint64_t words = m_meta.sources[k].words;
    if (from >= words || count == 0 || t.first.empty()) {
        return {};
    }
    count = static_cast<size_t>(std::min<uint64_t>(count, words - from));
    // The block that holds `from`.
    size_t b = static_cast<size_t>(std::upper_bound(t.first.begin(), t.first.end(), from) - t.first.begin()) - 1;
    if (t.pcm) {
        std::vector<int16_t> out;
        out.reserve(count);
        std::vector<uint8_t> buf;
        uint64_t at = from;
        for (; out.size() < count && b < t.first.size(); ++b) {
            const uint64_t skip = at - t.first[b];
            const uint64_t have = t.length[b] / 2 - skip;
            const size_t take = static_cast<size_t>(std::min<uint64_t>(have, count - out.size()));
            buf.resize(take * 2);
            m_file->readAt(buf.data(), buf.size(), t.offset[b] + skip * 2);
            for (size_t i = 0; i < take; ++i) {
                out.push_back(static_cast<int16_t>(buf[2 * i] | (buf[2 * i + 1] << 8)));
            }
            at += take;
        }
        return out;
    }
    const size_t last = static_cast<size_t>(std::upper_bound(t.first.begin(), t.first.end(), from + count - 1) - t.first.begin()) - 1;
    QTemporaryFile frames(QDir::tempPath() + QStringLiteral("/rfx-frames-XXXXXX.flac"));
    if (!frames.open()) {
        throw Error("cannot write a temporary file for the FLAC frames");
    }
    frames.write(reinterpret_cast<const char *>(t.header.data()), static_cast<qint64>(t.header.size()));
    std::vector<uint8_t> buf;
    for (size_t i = b; i <= last; ++i) {
        buf.resize(t.length[i]);
        m_file->readAt(buf.data(), buf.size(), t.offset[i]);
        frames.write(reinterpret_cast<const char *>(buf.data()), static_cast<qint64>(buf.size()));
    }
    frames.flush();
    QByteArray bytes;
    streamFfmpegOutput({QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-i"), frames.fileName(), QStringLiteral("-f"),
                        QStringLiteral("s16le"), QStringLiteral("-acodec"), QStringLiteral("pcm_s16le"), QStringLiteral("-")},
                       [&bytes](const char *data, size_t n) { bytes.append(data, static_cast<qsizetype>(n)); }, "ffmpeg could not decode the frames of " + m_path.toStdString());
    const size_t lead = static_cast<size_t>(from - t.first[b]);
    std::vector<int16_t> out;
    for (size_t i = lead; i < static_cast<size_t>(bytes.size()) / 2 && out.size() < count; ++i) {
        out.push_back(static_cast<int16_t>(static_cast<uint8_t>(bytes[static_cast<qsizetype>(2 * i)]) | (static_cast<uint8_t>(bytes[static_cast<qsizetype>(2 * i + 1)]) << 8)));
    }
    return out;
}

// A track of the FLAC or raw variant: its 16-bit samples, as ffmpeg decodes them, are turned into the canonical samples of the format the
// capture had (specification 9).
void TrackReader::readStored(size_t k, int64_t start, size_t n, int32_t *out, bool keep)
{
    readStretches(k, start, n, out, keep);
    const SourceMeta &sm = m_meta.sources[k];
    const SampleFormat format = sm.format();
    const int64_t from = std::max<int64_t>(start, 0);
    const int64_t to = std::min<int64_t>(start + static_cast<int64_t>(n), static_cast<int64_t>(sm.words));
    for (int64_t p = from; p < to; ++p) {
        out[p - start] = fromI16(format, static_cast<int16_t>(out[p - start]));
    }
}

// The samples of a compressed capture or of a track of the FLAC or raw variant of an .rfx file, from the stretches that are held, and
// from ffmpeg for those that are not.
void TrackReader::readStretches(size_t k, int64_t start, size_t n, int32_t *out, bool keep)
{
    // Where the samples are, and how many: one capture, whose header gives its rate, or a track of the variants, a stream of its file.
    const SourceMeta &sm = m_meta.sources[k];
    const double streamRate = m_compressed ? m_compressed->headerRate : std::max(1.0, std::round((sm.rateHz > 0 ? sm.rateHz : 75e6) / 1000.0));
    const uint64_t samples = m_compressed ? m_compressed->samples : sm.words;
    const auto decode = [&](uint64_t from, size_t count) { return m_stored ? storedWindow(k, from, count) : decodeWindow(m_path, streamRate, from, count); };
    constexpr uint64_t kStretch = uint64_t(1) << 18;
    // What a run of missing stretches may be, in one call of ffmpeg.
    constexpr uint64_t kMostPerCall = 8;
    // About 100 MB of decoded samples.
    constexpr size_t kMostHeld = 200;

    std::fill(out, out + n, 0);
    const int64_t total = static_cast<int64_t>(samples);
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
            covered = m_stretches.count({k, c}) != 0;
        }
        if (!covered) {
            const std::vector<int16_t> own = decode(static_cast<uint64_t>(from), static_cast<size_t>(to - from));
            for (size_t i = 0; i < own.size(); ++i) {
                out[from - start + static_cast<int64_t>(i)] = own[i];
            }
            return;
        }
    }

    // Decode the stretches that are missing, a run of them in one call.
    for (uint64_t c = first; c <= last;) {
        if (m_stretches.count({k, c}) != 0) {
            ++c;
            continue;
        }
        uint64_t end = c;
        while (end <= last && end - c < kMostPerCall && m_stretches.count({k, end}) == 0) {
            ++end;
        }
        const uint64_t run_samples = std::min<uint64_t>((end - c) * kStretch, samples - c * kStretch);
        const std::vector<int16_t> run = decode(c * kStretch, static_cast<size_t>(run_samples));
        for (uint64_t s = c; s < end; ++s) {
            const size_t lo = static_cast<size_t>((s - c) * kStretch);
            if (lo >= run.size()) {
                // (the file holds less than it said: what is not there is zero)
                m_stretches[{k, s}] = Stretch{std::make_shared<const std::vector<int16_t>>(), ++m_tick};
                continue;
            }
            const size_t hi = std::min<size_t>(run.size(), lo + static_cast<size_t>(kStretch));
            m_stretches[{k, s}] = Stretch{std::make_shared<const std::vector<int16_t>>(run.begin() + static_cast<std::ptrdiff_t>(lo), run.begin() + static_cast<std::ptrdiff_t>(hi)), ++m_tick};
        }
        c = end;
    }
    for (uint64_t c = first; c <= last; ++c) {
        Stretch &st = m_stretches[{k, c}];
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
