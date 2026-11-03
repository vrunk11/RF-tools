/******************************************************************************
 * convert.h
 * rfx library - giving the captures back in another format: other raw formats,
 * FLAC and Ogg-FLAC files (.flac, .ldf), and the tracks as the channels of FLAC
 * files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "metadata.h"
#include "sampleformat.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <string>

namespace rfx {

// What unpack writes when it is not the capture as it came (`--as`).
struct OutputFormat
{
    enum class Kind {
        // A raw format (`raw`).
        Raw,
        // FLAC (.flac) or Ogg-FLAC (.ldf), made by ffmpeg like ld-decode's own.
        Flac,
        Ldf,
        // The tracks as the channels of FLAC files, eight a file, to open in an audio program.
        MultichannelFlac
    };

    Kind kind = Kind::Raw;
    SampleFormat raw = SampleFormat::S16;
};

// Accepts s16, r16, s8, r8, lds, flac, ldf and mflac.
bool parseOutputFormat(const std::string &name, OutputFormat &out);

// A stored sample as a 16-bit one, or false when it does not fit.
bool toI16(SampleFormat fmt, int32_t v, int16_t &out);
// The inverse of toI16.
int32_t fromI16(SampleFormat fmt, int16_t s);

// Write the samples of `src` (in the format of `sm`) as a `target` file in
// `outdir`, named with the extension of the new format, check them by the hash of
// the samples, and remove `src`. Throws rfx::Error.
void convertFile(const QString &src, const SourceMeta &sm, SampleFormat target, const QString &outdir);

// Write the capture `src` as FLAC (or Ogg-FLAC, `.ldf`) with ffmpeg, the way
// ld-decode's tools do (mono, 16 bits, the container rate the RF rate in kHz),
// check that decoding it gives back the samples, and remove `src`.
void encodeFlacFile(const QString &src, const SourceMeta &sm, bool ogg, const QString &outdir);

// The tracks of a predictor archive (or only `only`) as the channels of FLAC files
// in `outdir`: one channel per track, eight channels a file, the sample rate the RF
// rate in kHz. A shorter track is padded with zeros; the tracks are not aligned on
// each other. The file is decoded again and compared with what was sent.
void exportMultichannelFlac(const QString &archive, const QString &outdir, std::optional<size_t> only);

} // namespace rfx
