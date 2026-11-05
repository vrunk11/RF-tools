/******************************************************************************
 * ffmpeg.h
 * rfx library - running ffmpeg, which does the FLAC and Matroska work
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rfx {

// The ffmpeg program: the FFMPEG environment variable, else `ffmpeg`.
QString ffmpegProgram();

// Run ffmpeg with `args` and wait; its messages go to this program's standard
// error. Throws rfx::Error with `failure` when it cannot start or fails.
void runFfmpeg(const QStringList &args, const std::string &failure);

// Run ffmpeg with `args`, whose standard output is decoded samples, and pass them
// to `sink` as they come. Throws rfx::Error with `failure` when it cannot start
// or fails.
// What ffmpeg reads in the header of a compressed capture: its sample rate as the header holds it (for the captures of
// ld-decode and the Domesday Duplicator, in kHz: 40000 for 40 MHz) and the bits of its samples. A rate or a bit depth it
// does not tell is 0. Nothing when ffmpeg cannot run or reads no audio.
struct CompressedHeader
{
    long headerRate = 0;
    int bits = 0;
    // How long it lasts in the time of its header, in seconds (0 when it is not said).
    double durationSeconds = 0.0;
};
std::optional<CompressedHeader> probeCompressed(const QString &path);

// What the header (STREAMINFO) of a native FLAC file says, exactly: ffmpeg gives the length to a hundredth of a second
// only, and this is the number of samples. Nothing for a file that is not native FLAC (an Ogg-FLAC one, say).
struct FlacInfo
{
    long sampleRate = 0;
    int channels = 0;
    int bits = 0;
    uint64_t samples = 0;
    // The sizes of the blocks (a frame holds that many samples), when they are all the same.
    int blockSize = 0;
};
std::optional<FlacInfo> readFlacInfo(const QString &path);

// The number of samples a native FLAC file really holds, found from its last frames: a file that was written to a pipe
// (as the captures of ld-decode are) has the length it started with in its header, or none, when it holds far more. Each
// frame says which sample it starts at; the last ones that follow each other are read from the end of the file. Nothing
// when no such run of frames is found.
std::optional<uint64_t> flacSamplesFromTail(const QString &path, const FlacInfo &info);

// `count` samples of a compressed capture from sample `start`, as 16-bit values, decoded by ffmpeg from where they are (it
// jumps there: a FLAC has marks to jump to). `headerRate` is the rate in the header of the file (the samples are in that
// time). Fewer come back past the end. `audioStream` (when not negative) picks the audio stream of a file that holds several, as the tracks
// of the FLAC and raw variants of .rfx are. Throws rfx::Error.
std::vector<int16_t> decodeWindow(const QString &path, double headerRate, uint64_t start, size_t count, int audioStream = -1);

void streamFfmpegOutput(const QStringList &args, const std::function<void(const char *, size_t)> &sink, const std::string &failure);

} // namespace rfx
