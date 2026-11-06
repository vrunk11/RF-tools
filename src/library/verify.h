/******************************************************************************
 * verify.h
 * rfx library - checking an archive against the checksums it keeps
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QString>

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rfx {

// What the check found for one track.
struct TrackCheck
{
    size_t track = 0;
    std::string name;
    // The samples decode to what was packed (SHA-256 of the samples), and the copies of the sidecar files are whole. False for the first thing
    // that is not (`detail` says which), and for a track that cannot be decoded.
    bool ok = false;
    // A checksum was there to compare with (an archive made before the samples were hashed has none: nothing is said of such a track).
    bool checked = false;
    std::string detail;
};

// Decode the tracks of the archive (`only`: just that one; the tracks it is coded against are decoded as it needs them) and compare what comes
// out with the SHA-256 of the samples the archive keeps, and each sidecar with its own. It works for the three variants of the file. The tracks are
// worked on together, so that what they are coded against is decoded once. `progress` is told how far the work is (0 to 1, from any thread);
// `stop`, when given and set, ends it early (the tracks not finished are then said not to be checked). Throws rfx::Error for a file that is not an
// archive.
std::vector<TrackCheck> verifyArchive(const QString &path, std::optional<size_t> only = std::nullopt, const std::function<void(double)> &progress = {},
                                      const std::atomic<bool> *stop = nullptr);

} // namespace rfx
