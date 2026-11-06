/******************************************************************************
 * flacmode.h
 * rfx library - the FLAC variant of the archive (specification 9): an ordinary
 * Matroska audio file, a FLAC track per capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "convert.h"
#include "edit.h"
#include "metadata.h"
#include "packer.h"

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace rfx {

// Whether `path` is a FLAC-variant archive (a Matroska file) rather than one of
// the predictor's (document type `rfx`).
bool isFlacArchive(const QString &path);

// How the samples of the tracks are stored in a Matroska variant: FLAC (smaller, lossless) or as raw 16-bit PCM (the samples as they
// are, in the same file that audio and video programs open).
enum class StoredCodec { Flac, Raw };

// What the attachment rfx.json holds (and, in `codec`, whether the tracks are "flac" or "raw"). Throws rfx::Error.
std::vector<SourceMeta> readFlacMeta(const QString &path, std::string *codec = nullptr, std::string *medium = nullptr);

// What is told of a track besides its samples, for the track at the same place of `inputs`: the name, the rate, the note, the date of
// its file, the bits it carries and the files that go with it, where they are not the ones the capture gives.
struct StoredTrackEdit
{
    std::optional<std::string> name;
    std::optional<double> rateHz;
    std::optional<std::string> notes;
    std::optional<std::string> fileTime;
    std::optional<uint32_t> bits;
    std::optional<std::vector<Sidecar>> sidecars;
    InfoEdit info;
};

// Pack `inputs` (in that order) as tracks of FLAC (or of raw PCM) in `output`; ffmpeg does the FLAC and the Matroska work.
// Throws rfx::Error.
void packFlac(const std::vector<QString> &inputs, const QString &output, const PackOptions &options, StoredCodec codec = StoredCodec::Flac,
              const std::vector<StoredTrackEdit> &edits = {});

// Write `output` in the variant `codec` from `picks`, in that order: tracks of the archives (of the predictor variant or of the FLAC or
// raw one, unpacked to be written again) and new captures; what a pick says to change (name, rate, note, files, bits) is changed. What
// the predictor does (levels, references) does not apply. When every pick is a track of a file of the variant that is asked for, with no change of rate, the
// streams are copied as they are (ffmpeg -c copy: no decoding, no encoding) and only the description of the tracks is written again. Throws rfx::Error.
void assembleStored(const std::vector<QString> &archives, const std::vector<Pick> &picks, const QString &output, const PackOptions &options, StoredCodec codec);

// What `rfx info` prints for such a file, and the whole metadata as YAML.
std::string flacInfo(const QString &path);
std::string flacYaml(const QString &path);

// Write the .yaml sidecars and <archive>.yaml (as unpackMetadata does).
void unpackFlacMetadata(const QString &path, const QString &outdir, std::optional<size_t> only);

// Give the captures back as they were, each checked by its SHA-256; with
// `target`, then in that format (checked by the hash of the samples).
void unpackFlac(const QString &path, const QString &outdir, std::optional<size_t> only, const std::optional<OutputFormat> &target);

} // namespace rfx
