/******************************************************************************
 * metadata.h
 * rfx library - the metadata block of an .rfx file (specification 3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "predictor.h"
#include "sampleformat.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfx {

constexpr const char *kFormatName = "rfx";
constexpr uint32_t kFormatVersion = 1;

constexpr const char *kCodecPred = "D_RFX/PRED";
constexpr const char *kCodecMeta = "D_RFX/META";
constexpr const char *kCodecMap = "D_RFX/MAP";

// A small file stored whole in the metadata and given back by unpack.
struct Sidecar
{
    std::string name;
    std::string sha256;
    std::vector<uint8_t> data;
};

// What the archive knows about one track (one capture).
struct SourceMeta
{
    // File name of the capture (no directory).
    std::string name;
    // Bytes of the original file.
    uint64_t size = 0;
    std::string sha256;
    // Samples in the file; the bytes past the last whole group are in `tail`.
    uint64_t words = 0;
    std::vector<uint8_t> tail;
    // "s16", "r16", "s8", "r8" or "lds".
    std::string fmt = "s16";
    // SHA-256 of the canonical samples as little-endian i32, to check a file
    // written in another format.
    std::string sampleSha256;
    // Low bits that are zero in every sample.
    uint32_t shift = 0;
    int32_t mean = 0;
    // Predictor parameters (specification 5.1).
    size_t own = 0;
    size_t solo = 0;
    size_t taps = 0;
    size_t ntop = 0;
    size_t ttaps = 0;
    size_t stride = 1;
    size_t every = 1;
    uint32_t lamBits = 0;
    // The earlier tracks this one is predicted from, and the weight of each in
    // the mean they make (empty means equal weights).
    std::vector<size_t> refs;
    std::vector<float> weights;
    // Where the capture stood on the command line when packed (informational).
    size_t position = 0;
    // Noise against signal used to order the tracks (informational).
    std::optional<double> noise;
    std::vector<Sidecar> sidecars;
    // The compressed file the capture was decoded from, if it was.
    std::optional<std::string> origin;
    // Sample rate in Hz; 0 when unknown.
    double rateHz = 0;
    // A note about the capture, free text, for people (empty: none).
    std::string notes;
    // Bits the samples carry (the rest of each 16-bit sample is zero), as the person who made the file says it; 0 when not said.
    uint32_t bits = 0;
    // When the file the capture was read from was last changed, as UTC in ISO 8601
    // ("2023-05-16T18:03:54Z"), when it was packed.
    std::optional<std::string> fileTime;

    SampleFormat format() const;
    Preset preset() const;
    // The `ntop` references with channels of their own, heaviest first (ties by
    // place in `refs`).
    std::vector<size_t> tops() const;
};

struct ArchiveMeta
{
    std::string format = kFormatName;
    uint32_t version = kFormatVersion;
    // Samples per segment.
    size_t segment = 0;
    std::vector<SourceMeta> sources;
};

// Throws rfx::Error for a document that is not valid or not of this format.
ArchiveMeta parseMeta(const uint8_t *data, size_t size);
std::vector<uint8_t> serializeMeta(const ArchiveMeta &meta);

// The metadata of the FLAC variant (the attachment rfx.json, specification 9): one
// entry per audio track, with the predictor fields zero or empty.
std::vector<SourceMeta> parseFlacMeta(const uint8_t *data, size_t size);
std::vector<uint8_t> serializeFlacMeta(const std::vector<SourceMeta> &tracks);

// Lower-case hex of `bytes`.
std::string toHex(const uint8_t *bytes, size_t size);

} // namespace rfx
