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
#include <functional>
#include <string>

namespace rfx {

// The ffmpeg program: the FFMPEG environment variable, else `ffmpeg`.
QString ffmpegProgram();

// Run ffmpeg with `args` and wait; its messages go to this program's standard
// error. Throws rfx::Error with `failure` when it cannot start or fails.
void runFfmpeg(const QStringList &args, const std::string &failure);

// Run ffmpeg with `args`, whose standard output is decoded samples, and pass them
// to `sink` as they come. Throws rfx::Error with `failure` when it cannot start
// or fails.
void streamFfmpegOutput(const QStringList &args, const std::function<void(const char *, size_t)> &sink, const std::string &failure);

} // namespace rfx
