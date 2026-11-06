/******************************************************************************
 * archive.h
 * rfx library - reading an .rfx archive: its metadata, lag tables and blocks,
 * and unpacking it (specification 2 to 6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "convert.h"
#include "device.h"
#include "alignment.h"
#include "lagtable.h"
#include "metadata.h"
#include "posfile.h"

#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rfx {

// Where the coded bytes of one segment are in the file.
struct BlockSpan
{
    uint64_t offset = 0;
    uint64_t length = 0;
};

// What pass 0 found in the RF of a track (the fields: their frame codes and where each line is), from which the speed of the disc and the
// time base correction are made. Kept in the archive, optionally (specification 3.1), by tracks that were measured.
struct SavedPass0
{
    bool pal = false;
    double rateHz = 0.0;
    std::vector<Field> fields;
};

// An opened predictor archive. Throws rfx::Error for a file that is not one.
class Archive
{
public:
    explicit Archive(const QString &path);

    const ArchiveMeta &meta() const { return m_meta; }
    const std::map<std::pair<size_t, size_t>, LagTable> &tables() const { return m_tables; }
    // The segments of track `k`, by segment number.
    const std::unordered_map<uint64_t, BlockSpan> &blocks(size_t k) const { return m_blocks[k]; }
    PosFile &file() { return *m_file; }
    // The TBC curves the archive keeps, by track (none when it keeps none).
    const std::map<size_t, SavedPass0> &pass0() const { return m_pass0; }

    // Bytes the coded segments of track `k` take.
    uint64_t packedBytes(size_t k) const;

private:
    ArchiveMeta m_meta;
    std::map<std::pair<size_t, size_t>, LagTable> m_tables;
    std::vector<std::unordered_map<uint64_t, BlockSpan>> m_blocks;
    std::map<size_t, SavedPass0> m_pass0;
    std::unique_ptr<PosFile> m_file;
};

// Keep TBC curves in the archive, by track: a block added at the end of the file (nothing else is rewritten), which replaces the ones
// kept before. Throws rfx::Error when the file is not a predictor archive or cannot be written.
void savePass0(const QString &path, const std::map<size_t, SavedPass0> &tracks);

// What `rfx info` prints.
std::string archiveInfo(const QString &path);

// The whole metadata of an archive as YAML: the archive's own fields, every
// track's (its predictor, references and weights, format, hashes, the rate,
// the bytes past the last whole sample as hex), the .yaml sidecars as text and
// the size each track takes in the archive.
std::string archiveYaml(const QString &path);

// Rebuild the original files in `outdir`, each checked against its SHA-256.
// With `only`, just that track (the earlier ones are decoded too, as its
// references, and removed). Progress goes to `log`.
struct UnpackOptions
{
    std::optional<size_t> only;
    // Write the captures in another format as well (see OutputFormat); the
    // multichannel FLAC one has its own function, exportMultichannelFlac.
    std::optional<OutputFormat> target;
    // Where the predictor runs.
    Device device = Device::Cpu;
    // Threads decoding segments; 0 for one per processor.
    unsigned threads = 0;
};
void unpackArchive(const QString &path, const QString &outdir, const UnpackOptions &options);

// Write what the archive keeps besides the samples, without decoding any: each
// track's .yaml sidecars, as they were (checked by hash), and <archive>.yaml,
// the whole metadata. Prints the paths written.
void unpackMetadata(const QString &path, const QString &outdir, std::optional<size_t> only);

} // namespace rfx
