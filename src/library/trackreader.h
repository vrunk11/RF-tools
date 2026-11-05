/******************************************************************************
 * trackreader.h
 * rfx library - reading the samples of the tracks of an .rfx file, or of a raw
 * capture, at any place, decoding only what is asked for
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "archive.h"
#include "posfile.h"
#include "refwindow.h"
#include "sampleformat.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rfx {

// What a raw capture does not say of itself, and a person has to.
struct CaptureSpec
{
    SampleFormat format = SampleFormat::S16;
    // The sample rate in Hz.
    double rateHz = 0.0;
    // The bits the samples carry; found from the samples when not given.
    std::optional<int> bits;
    // The name to show; the file's own when empty.
    std::string name;
    // A compressed capture (FLAC, 16-bit): it is not decoded to a file, its samples are decoded where they are asked for,
    // by ffmpeg, which jumps to them. `rateHz` is the real rate (the header of the file holds it in kHz).
    bool compressed = false;
};

// The shift a capture of `format` carrying `bits` has (specification 1).
uint32_t shiftForBits(SampleFormat format, int bits);

// A predictor archive, or a raw capture (one track with no reference), opened to be
// looked at: the samples of any track over any range. For an archive they come from
// the segments that hold them, decoded when asked (and the segments of the
// references they are predicted from, as far as the window needs them) and kept
// for the next question. read() can be called from several threads.
class TrackReader
{
public:
    // Throws rfx::Error for a file that is not a predictor archive.
    explicit TrackReader(const QString &path);
    // A raw capture. Throws rfx::Error for a file that cannot be read or that holds
    // no sample.
    TrackReader(const QString &path, const CaptureSpec &spec);

    // A predictor archive, or an .rfx file of the FLAC or raw variant (whose tracks have no reference: they are decoded by ffmpeg
    // where they are looked at).
    bool isArchive() const { return m_archive != nullptr || m_stored; }
    const QString &path() const { return m_path; }
    // The metadata of the tracks; for a raw capture one track made from what was
    // given and what was found (the name, the size, the samples, the shift, the rate).
    const ArchiveMeta &meta() const { return m_meta; }
    size_t trackCount() const { return m_meta.sources.size(); }
    // The size of the file, and the bytes the coded segments of track `k` take (the
    // file's size for a raw capture).
    uint64_t fileSize() const { return m_fileSize; }
    uint64_t packedBytes(size_t k) const { return m_archive ? m_archive->packedBytes(k) : (m_stored ? m_fileSize / std::max<size_t>(1, m_meta.sources.size()) : m_fileSize); }
    // The lag table of track `k` against its reference `j`, or null.
    const LagTable *lagTable(size_t k, size_t j) const;

    // The `n` canonical samples of track `k` from sample `start` (zero outside the
    // track), in the sense of specification 1.
    void read(size_t k, int64_t start, size_t n, int32_t *out);
    // The same without keeping what is decoded for the next question (for a compressed capture: a look at the capture here
    // and there must not push out the stretch that is being worked on). Else as read().
    void readOnce(size_t k, int64_t start, size_t n, int32_t *out);

    // A capture that is decoded as it is looked at.
    bool isCompressed() const { return m_compressed.has_value(); }

private:
    void readStretches(size_t k, int64_t start, size_t n, int32_t *out, bool keep);
    void readStored(size_t k, int64_t start, size_t n, int32_t *out, bool keep);
    using Segment = std::shared_ptr<const std::vector<int32_t>>;
    Segment segment(size_t k, uint64_t g);

    struct Held
    {
        Segment samples;
        uint64_t tick = 0;
    };

    QString m_path;
    uint64_t m_fileSize = 0;
    std::unique_ptr<Archive> m_archive;
    std::unique_ptr<PosFile> m_file;
    ArchiveMeta m_meta;
    std::map<std::pair<size_t, size_t>, LagTable> m_noTables;
    std::unique_ptr<ReferenceWindows> m_windows;
    std::mutex m_mutex;

    // The compressed capture: the rate of its header, its samples, and the stretches that are decoded (each of 2^18
    // samples), the most recently used kept.
    struct Compressed
    {
        double headerRate = 0.0;
        uint64_t samples = 0;
    };
    struct Stretch
    {
        std::shared_ptr<const std::vector<int16_t>> samples;
        uint64_t tick = 0;
    };
    std::optional<Compressed> m_compressed;
    std::map<std::pair<size_t, uint64_t>, Stretch> m_stretches;
    // An .rfx file of the FLAC or raw variant: its tracks are the audio of a Matroska file. The blocks of each track are indexed when the
    // file is opened (the first sample of each, and where its payload is), so that any sample is read exactly, from the frames or the PCM
    // that hold it, without the seeking of ffmpeg (which goes by timestamps of a millisecond).
    bool m_stored = false;
    struct StoredTrack
    {
        std::vector<uint64_t> first;
        std::vector<uint64_t> offset;
        std::vector<uint32_t> length;
        // FLAC: "fLaC" and the STREAMINFO block (made the last metadata block), and the samples a frame holds; PCM: no header.
        std::vector<uint8_t> header;
        uint64_t frameSamples = 0;
        bool pcm = false;
    };
    std::vector<StoredTrack> m_storedTracks;
    void indexStored();
    std::vector<int16_t> storedWindow(size_t k, uint64_t from, size_t count);
    std::map<std::pair<size_t, uint64_t>, Held> m_held;
    uint64_t m_tick = 0;
};

} // namespace rfx
