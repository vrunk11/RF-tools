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
    // Capture sample rate in MHz, for pass 0 and for the metadata when the
    // capture has no .yaml.
    double rateMhz = 75.0;
    // "auto" (from the extension), s16, r16, s8, r8, lds, or flac / ldf (decoded
    // with ffmpeg to raw 16-bit samples first).
    std::string format = "auto";
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

} // namespace rfx
