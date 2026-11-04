/******************************************************************************
 * capture.h
 * rfx library - the capture files given to pack: their statistics, sidecars,
 * format and compressed inputs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "metadata.h"
#include "posfile.h"
#include "sampleformat.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfx {

// What pack learns about a capture by reading it whole.
struct CaptureStats
{
    uint64_t size = 0;
    // SHA-256 of the file's bytes.
    std::string sha;
    // Low bits that are zero in every sample, and the rounded mean of the samples
    // shifted right by them (specification 1).
    uint32_t shift = 0;
    int32_t mean = 0;
    // SHA-256 of the samples as little-endian i32.
    std::string sampleSha;
};

CaptureStats captureStats(const QString &path, SampleFormat fmt);
// The same for a file already open: opening a QFile in a worker thread costs a
// tenth of a second or more, so a caller that scans files in parallel opens them first.
CaptureStats captureStats(PosFile &file, SampleFormat fmt);

// SHA-256 of `count` samples as little-endian i32.
std::string hashSamples(PosFile &file, SampleFormat fmt, uint64_t count);

// SHA-256 of `size` bytes of the file.
std::string hashBytes(PosFile &file, uint64_t size);

// The file that sits beside a capture (`x.ddd.s16` -> `x.ddd.yaml`): the
// Domesday Duplicator's metadata, kept whole if it is there and small.
std::vector<Sidecar> findSidecars(const QString &capture);

// The sample rate in Hz that a capture's .yaml gives, if one of the sidecars does.
std::optional<double> sidecarRateHz(const std::vector<Sidecar> &sidecars);

// When a capture was made, from its .yaml (the Domesday Duplicator writes "started", "finished" and "duration_seconds"):
// the times are local, as written, in ISO 8601 without a zone ("2026-10-27T07:33:55").
struct CaptureTimes
{
    std::string started;
    std::string finished;
    // 0 when the .yaml does not say.
    double durationSeconds = 0.0;
};
std::optional<CaptureTimes> captureTimes(const std::vector<Sidecar> &sidecars);

// What the .yaml of the Domesday Duplicator says, as it is laid out: the lines at the left edge that have a value, in a section named
// "file", then a section for each key that opens one ("capture", "signal", "requantization", "board", "device", "player", "disc"...),
// the rows of each as they are written (key and value, the quotes taken off). Nothing for a sidecar that is not such a file.
struct SidecarSection
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> rows;
};
std::vector<SidecarSection> sidecarSections(const std::vector<Sidecar> &sidecars);

// What the capture declares of its samples, enough to open it without asking: the format ("signed 16-bit" or "FLAC") and the rate in Hz.
// The bits the samples carry are found from the samples: its "bit_shift" is a shift of the converter's own bits, not their number.
// Nothing when the format or the rate is not there.
struct DeclaredCapture
{
    SampleFormat format = SampleFormat::S16;
    bool compressed = false;
    double rateHz = 0.0;
};
std::optional<DeclaredCapture> declaredCapture(const std::vector<Sidecar> &sidecars);

// The format of an input: `format` ("auto" guesses from the extension; "ldf" and
// "flac" give s16, as they are decoded to it). Throws rfx::Error.
SampleFormat inputFormat(const QString &path, const std::string &format);

// `path` with its last extension replaced by `extension` (added if it has none).
QString withExtension(const QString &path, const QString &extension);

// A compressed capture (FLAC or Ogg-FLAC) goes through ffmpeg to raw samples
// first. Returns the list to pack, and the work directory made beside `out` for
// the raw files, if one was made. Throws rfx::Error.
struct DecodedInputs
{
    std::vector<QString> paths;
    QString workDir;
};
DecodedInputs decodeCompressed(const std::vector<QString> &given, const QString &out, const std::string &format);

} // namespace rfx
