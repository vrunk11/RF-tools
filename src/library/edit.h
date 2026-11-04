/******************************************************************************
 * edit.h
 * rfx library - editing archives: merge several into one, extract some of the
 * tracks into another, remove tracks, add captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "packer.h"

#include "metadata.h"

#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

namespace rfx {

// What is changed in a track on the way: for one of an archive its name, rate, note and sidecar files (nothing is coded
// for that), and how it is written; for a new capture the rate, the note and the level.
struct TrackEdit
{
    std::optional<std::string> name;
    std::optional<double> rateHz;
    // An empty note takes the note away.
    std::optional<std::string> notes;
    // The bits the samples carry, as the metadata says it.
    std::optional<uint32_t> bits;
    std::optional<std::vector<Sidecar>> sidecars;
    // Code the track again even when it could be copied as it is.
    bool recode = false;
    // The level (1 to 4) it is coded at when it is coded; the options' when not given.
    std::optional<int> level;
};

// One track of the archive to write: track `src` of the archive number `arch`
// in the list given, or a capture file.
struct Pick
{
    bool fromArchive = true;
    size_t arch = 0;
    size_t src = 0;
    QString capture;
    TrackEdit edit;

    static Pick old(size_t arch, size_t src)
    {
        Pick p;
        p.arch = arch;
        p.src = src;
        return p;
    }
    static Pick fresh(const QString &capture)
    {
        Pick p;
        p.fromArchive = false;
        p.capture = capture;
        return p;
    }
};

// Write `output` (always a new file) from `picks`, in that order. A track is
// coded against the tracks before it, so its coded blocks stay valid exactly
// when every track it was coded against is still there, before it: those are
// copied as they are, which costs no more than the copy. The others, and the new
// captures, are coded again against what is left. `recodeFrom`: the archives from
// that one on are coded again against everything before them, so that captures of
// one disc in different archives predict each other. Throws rfx::Error.
void assembleArchives(const std::vector<QString> &archives, const std::vector<Pick> &picks, const QString &output,
                      const PackOptions &options, std::optional<size_t> recodeFrom);

// The tracks of `archive` listed in `which` (0 is the primary), in their order.
std::vector<Pick> pickSome(const QString &archive, const std::vector<size_t> &which);
// All the tracks of `archive` except `which`.
std::vector<Pick> pickWithout(const QString &archive, const std::vector<size_t> &which);
// Every track of every archive, one archive after the other.
std::vector<Pick> pickAll(const std::vector<QString> &archives);

} // namespace rfx
