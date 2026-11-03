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
struct CaptureSpec
{
    std::optional<SourceMeta> carry;
    bool frozen = false;
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

// [`known`: the lag tables of the frozen inputs, by their places in `given`.]
std::unique_ptr<PreparedPack> prepareCaptures(const std::vector<QString> &given, const std::vector<CaptureSpec> &specs,
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
