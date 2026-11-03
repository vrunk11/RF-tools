/******************************************************************************
 * ffmpeg.cpp
 * rfx library - running ffmpeg, which does the FLAC and Matroska work
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "ffmpeg.h"

#include "error.h"

#include <QProcess>
#include <QProcessEnvironment>

namespace rfx {

QString ffmpegProgram()
{
    const QString fromEnv = QProcessEnvironment::systemEnvironment().value(QStringLiteral("FFMPEG"));
    return fromEnv.isEmpty() ? QStringLiteral("ffmpeg") : fromEnv;
}

void runFfmpeg(const QStringList &args, const std::string &failure)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::ForwardedChannels);
    proc.start(ffmpegProgram(), args);
    if (!proc.waitForStarted()) {
        throw Error("starting " + ffmpegProgram().toStdString());
    }
    proc.waitForFinished(-1);
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        throw Error(failure);
    }
}

void streamFfmpegOutput(const QStringList &args, const std::function<void(const char *, size_t)> &sink, const std::string &failure)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    proc.start(ffmpegProgram(), args);
    if (!proc.waitForStarted()) {
        throw Error("starting " + ffmpegProgram().toStdString());
    }
    while (proc.state() != QProcess::NotRunning || proc.bytesAvailable() > 0) {
        if (proc.bytesAvailable() == 0 && !proc.waitForReadyRead(-1) && proc.bytesAvailable() == 0) {
            break;
        }
        const QByteArray chunk = proc.readAllStandardOutput();
        if (!chunk.isEmpty()) {
            sink(chunk.constData(), static_cast<size_t>(chunk.size()));
        }
    }
    proc.waitForFinished(-1);
    const QByteArray rest = proc.readAllStandardOutput();
    if (!rest.isEmpty()) {
        sink(rest.constData(), static_cast<size_t>(rest.size()));
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        throw Error(failure);
    }
}

} // namespace rfx
