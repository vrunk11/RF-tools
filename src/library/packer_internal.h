/******************************************************************************
 * packer_internal.h
 * rfx library - the two halves of packing (what is worked out before anything
 * is written, and the writing), shared by pack and the editing commands
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "archive.h"
#include "lagtable.h"
#include "metadata.h"
#include "packer.h"
#include "posfile.h"

#include <QDir>
#include <QString>

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace rfx {

using LagTables = std::map<std::pair<size_t, size_t>, LagTable>;

// How an input stands when an archive is edited: carried over from another
// archive (`carry`: its metadata there), and whether its coding can be copied as
// it is (`frozen`: every reference it was coded with is still there, before it).
struct PackSpec
{
    std::optional<SourceMeta> carry;
    bool frozen = false;
    // The capture is the samples of a track of another archive changed one by one
    // (requantised), so its lines are where that track's were: `like` is that
    // track, with its references renumbered for this archive, and the lag tables
    // against them are in `known`. The capture is hashed and coded afresh, with
    // the references, weights and predictor parameters of `like`; no line is
    // located again.
    std::optional<SourceMeta> like;
    // What a person asked for this track (see TrackEdit): the level it is coded at, the rate and the note it gets (not for a track
    // carried over from an archive: its `carry` has them already).
    std::optional<int> level;
    std::optional<double> rateHz;
    std::optional<std::string> notes;
    std::optional<uint32_t> bits;
    InfoEdit info;

    // Its references and parameters are given, not worked out.
    bool settled() const { return frozen || like.has_value(); }
};

// What pack works out before it writes anything: the captures in coding order,
// the metadata, the lag tables and the open files.
struct PreparedPack
{
    ~PreparedPack()
    {
        fileHolders.clear();
        files.clear();
        if (!workDir.isEmpty()) {
            QDir(workDir).removeRecursively();
        }
    }

    ArchiveMeta meta;
    LagTables tables;
    // Where the lines of each track are, in the order of the tracks, when they were located (they are kept in the archive, with keepPass0).
    std::vector<std::vector<Field>> fields;
    bool pal = false;
    double fieldsRateHz = 0.0;
    std::vector<std::unique_ptr<PosFile>> fileHolders;
    std::vector<PosFile *> files;
    // Raw files made from compressed captures, removed with this.
    QString workDir;
};

// A track whose coded blocks come from another archive instead of being coded.
struct CopiedTrack
{
    Archive *archive = nullptr;
    size_t track = 0;
};

// [`known`: the lag tables of the frozen and `like` inputs, by their places in `given`.]
std::unique_ptr<PreparedPack> prepareCaptures(const std::vector<QString> &given, const std::vector<PackSpec> &specs,
                                              const LagTables &known, const QString &output, const PackOptions &options);

// Write the archive for `prepared`; the tracks with a place in `copy` take their
// coded blocks from there instead of being coded again.
std::vector<PackedTrack> writePrepared(PreparedPack &prepared, const QString &output, const PackOptions &options,
                                       const std::vector<std::optional<CopiedTrack>> &copy);

// The head of an archive (tracks, metadata, lag tables) and the coded blocks of
// track `src` of `archive` as track `k`, to a file being written.
void writeArchiveHead(QFile &out, const ArchiveMeta &meta, const LagTables &tables);
uint64_t copyBlocks(QFile &out, Archive &archive, size_t src, size_t k);

} // namespace rfx
