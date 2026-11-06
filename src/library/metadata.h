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

// What a person says of the disc and of how the capture was made, per track (nothing is guessed from the samples): the video standard, a
// reference of the disc, the number of the copy and the face, and whether the signal is composite (CVBS) or separate luma and chroma (Y/C).
// The values the standard, the face and the signal take (lower case, but for the face), and the empty string for "not said".
constexpr const char *kStandards[] = {"pal", "ntsc", "pal-m", "secam", "ntsc-vhd"};
constexpr const char *kFaces[] = {"A", "B", "AB", "BA"};
constexpr const char *kSignals[] = {"cvbs", "yc"};
// The value as the file writes it (lower case for the standard and the signal, upper case for the face), or false when it is none of them.
bool parseStandard(const std::string &text, std::string &out);
bool parseFace(const std::string &text, std::string &out);
bool parseSignal(const std::string &text, std::string &out);

// What a person changes of that in a track: what is not given is left as it is; an empty text, or 0 for the copy, takes the value away.
struct InfoEdit
{
    std::optional<std::string> standard;
    std::optional<std::string> reference;
    std::optional<uint32_t> copy;
    std::optional<uint32_t> capture;
    std::optional<std::string> face;
    std::optional<std::string> signal;

    bool any() const { return standard || reference || copy || capture || face || signal; }
    // What `over` changes, on top of what this changes.
    void overlay(const InfoEdit &over);
    // What this says of a track as it is (every field, the empty ones too).
    static InfoEdit of(const struct SourceMeta &sm);
};

// What the archive knows about one track (one capture).
struct SourceMeta;
// What a person said of the disc and the signal, as a line of text (for `rfx info`): "standard ntsc, reference ..., copy 2, face A, signal CVBS".
std::string describeInfo(const SourceMeta &sm);

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
    // The disc and the signal, as a person said them (empty, and 0, when not said): see kStandards, kFaces and kSignals.
    std::string standard;
    std::string reference;
    // `copy` is the number of the physical copy of the disc (the exemplar the capture was made from), `capture` the number of the capture itself (a
    // copy can be captured several times).
    uint32_t copy = 0;
    uint32_t capture = 0;
    std::string face;
    std::string signal;

    // What a person changes of them.
    void apply(const InfoEdit &edit);
    bool hasInfo() const { return !standard.empty() || !reference.empty() || copy > 0 || capture > 0 || !face.empty() || !signal.empty(); }

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
// The variant stores its tracks as FLAC (`codec` "flac") or as raw PCM (`codec` "raw"); parse says which in `codecOut`.
std::vector<SourceMeta> parseFlacMeta(const uint8_t *data, size_t size, std::string *codecOut = nullptr);
std::vector<uint8_t> serializeFlacMeta(const std::vector<SourceMeta> &tracks, const char *codec = "flac");

// Lower-case hex of `bytes`.
std::string toHex(const uint8_t *bytes, size_t size);

} // namespace rfx
