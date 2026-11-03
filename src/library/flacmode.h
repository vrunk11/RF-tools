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

// What the attachment rfx.json holds. Throws rfx::Error.
std::vector<SourceMeta> readFlacMeta(const QString &path);

// Pack `inputs` (in that order) as FLAC tracks in `output`; ffmpeg does the FLAC
// and the Matroska work. Throws rfx::Error.
void packFlac(const std::vector<QString> &inputs, const QString &output, const PackOptions &options);

// What `rfx info` prints for such a file, and the whole metadata as YAML.
std::string flacInfo(const QString &path);
std::string flacYaml(const QString &path);

// Write the .yaml sidecars and <archive>.yaml (as unpackMetadata does).
void unpackFlacMetadata(const QString &path, const QString &outdir, std::optional<size_t> only);

// Give the captures back as they were, each checked by its SHA-256; with
// `target`, then in that format (checked by the hash of the samples).
void unpackFlac(const QString &path, const QString &outdir, std::optional<size_t> only, const std::optional<OutputFormat> &target);

} // namespace rfx
