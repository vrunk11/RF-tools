/******************************************************************************
 * packer.h
 * rfx library - packing captures into an .rfx archive
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "device.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfx {

// In which order the captures are coded.
enum class OrderMode {
    // The order on the command line.
    Given,
    // Each group of captures of one programme from the cleanest to the noisiest.
    Auto
};

struct PackOptions
{
    OrderMode order = OrderMode::Auto;
    // Predictor strength: 1 fastest to 4 strongest.
    int level = 3;
    // Samples per independently coded segment.
    size_t segment = 262144;
    // Decode every segment again and compare it with its samples.
    bool verify = true;
    // Directory of pass 0 dumps (pass0_<i>.txt, in the order of the inputs), if
    // already made.
    std::optional<QString> pass0Dir;
    QString ldDecode = QStringLiteral("ld-decode");
    // Run ld-decode (`ldDecode`) for pass 0, as the first versions did, instead of locating the lines in the process (rfsync.h), which gives
    // the same positions.
    bool externalPass0 = false;
    // Locate the lines of every track from its RF once the archive is written and keep them in it (keepPass0, rfsync.h): the speed of the
    // disc and the time base correction are then there without the minutes of work each time. Done by the program that asks, not by
    // packCaptures.
    bool keepPass0 = false;
    // The captures are of a PAL disc (NTSC when not).
    bool pal = false;
    // Capture sample rate in MHz, for pass 0 and for the metadata when the
    // capture has no .yaml.
    double rateMhz = 75.0;
    // "auto" (from the extension), s16, r16, s8, r8, lds, or flac / ldf (decoded
    // with ffmpeg to raw 16-bit samples first).
    std::string format = "auto";
    // Where the predictor runs.
    Device device = Device::Cpu;
    // Threads coding segments; 0 for one per processor.
    unsigned threads = 0;
};

struct PackedTrack
{
    std::string name;
    uint64_t size = 0;
    uint64_t packed = 0;
    size_t refs = 0;
};

// Pack `inputs` (the first is the primary, unless the order is changed) into
// the archive `output`. Progress goes to stderr. Throws rfx::Error.
std::vector<PackedTrack> packCaptures(const std::vector<QString> &inputs, const QString &output, const PackOptions &options);

// Pack `inputs` as `tracks` of the archive `archive` (in its order, one capture
// for each), which they are the samples of, changed one by one (requantised): the
// lines are where they were, so the references, weights, predictor parameters and
// lag tables of the archive are kept and no line is located again (no pass 0, no
// ld-decode). A reference that is not among `tracks` is dropped, with its weight.
// The segments are the archive's, and so are the sample rates, unless `rateHz` gives
// one. Of `options`, the device, the threads and the check are used. Throws rfx::Error.
std::vector<PackedTrack> packCapturesLike(const std::vector<QString> &inputs, const QString &archive, const std::vector<size_t> &tracks,
                                          const QString &output, const PackOptions &options, std::optional<double> rateHz = std::nullopt);

} // namespace rfx
