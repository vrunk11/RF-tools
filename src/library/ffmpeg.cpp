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
#include <QFile>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <algorithm>
#include <cstring>
#include <vector>

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

std::optional<CompressedHeader> probeCompressed(const QString &path)
{
    // `ffmpeg -i file` with no output says what is in the file, on its standard error, and stops.
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(ffmpegProgram(), {QStringLiteral("-hide_banner"), QStringLiteral("-i"), path});
    if (!proc.waitForStarted()) {
        return std::nullopt;
    }
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        proc.waitForFinished();
        return std::nullopt;
    }
    const QString text = QString::fromUtf8(proc.readAll());
    static const QRegularExpression audio(QStringLiteral(R"(Stream #[^\n]*Audio:[^\n]*)"));
    const QRegularExpressionMatch line = audio.match(text);
    if (!line.hasMatch()) {
        return std::nullopt;
    }
    const QString s = line.captured(0);
    CompressedHeader h;
    static const QRegularExpression duration(QStringLiteral(R"(Duration: (d+):(d+):(d+(?:.d+)?))"));
    const QRegularExpressionMatch d = duration.match(text);
    if (d.hasMatch()) {
        h.durationSeconds = d.captured(1).toDouble() * 3600.0 + d.captured(2).toDouble() * 60.0 + d.captured(3).toDouble();
    }
    static const QRegularExpression rate(QStringLiteral(R"((\d+) Hz)"));
    const QRegularExpressionMatch r = rate.match(s);
    if (r.hasMatch()) {
        h.headerRate = r.captured(1).toLong();
    }
    // "s16", "s32 (24 bit)", "s16p": the bits of the samples, the real ones when it says them.
    static const QRegularExpression real(QStringLiteral(R"(\((\d+) bit\))"));
    static const QRegularExpression fmt(QStringLiteral(R"(\bs(\d+)p?\b)"));
    const QRegularExpressionMatch b = real.match(s);
    const QRegularExpressionMatch f = fmt.match(s);
    h.bits = b.hasMatch() ? b.captured(1).toInt() : (f.hasMatch() ? f.captured(1).toInt() : 0);
    return h;
}

std::optional<FlacInfo> readFlacInfo(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray head = file.read(42);
    // "fLaC", the header of the first block (STREAMINFO, 34 bytes), then its fields: 16 + 16 bits of block sizes, 24 + 24
    // of frame sizes, then 20 bits of sample rate, 3 of channels (less one), 5 of bits per sample (less one) and 36 of
    // samples.
    if (head.size() < 42 || head.left(4) != "fLaC" || (static_cast<unsigned char>(head[4]) & 0x7F) != 0) {
        return std::nullopt;
    }
    const auto b = [&](int i) { return static_cast<uint64_t>(static_cast<unsigned char>(head[i])); };
    FlacInfo info;
    info.sampleRate = static_cast<long>((b(18) << 12) | (b(19) << 4) | (b(20) >> 4));
    info.channels = static_cast<int>(((b(20) >> 1) & 0x7) + 1);
    info.bits = static_cast<int>((((b(20) & 1) << 4) | (b(21) >> 4)) + 1);
    info.samples = ((b(21) & 0xF) << 32) | (b(22) << 24) | (b(23) << 16) | (b(24) << 8) | b(25);
    const int minBlock = static_cast<int>((b(8) << 8) | b(9));
    const int maxBlock = static_cast<int>((b(10) << 8) | b(11));
    info.blockSize = minBlock == maxBlock ? maxBlock : 0;
    return info;
}

namespace {

// The CRC-8 of a FLAC frame header (polynomial x^8 + x^2 + x + 1).
uint8_t crc8(const unsigned char *data, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1));
        }
    }
    return crc;
}

struct FrameAt
{
    uint64_t start = 0;
    uint64_t end = 0;
};

// The frame header at `p` (`n` bytes are there), or no frame when what is there is not one (the CRC says so).
bool frameHeader(const unsigned char *p, size_t n, const FlacInfo &info, FrameAt &frame)
{
    if (n < 16 || p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) {
        return false;
    }
    const bool variable = (p[1] & 1) != 0;
    const int blockCode = p[2] >> 4;
    const int rateCode = p[2] & 0xF;
    if (blockCode == 0 || rateCode == 15 || (p[3] >> 4) > 10 || ((p[3] >> 1) & 7) == 3 || (p[3] & 1) != 0) {
        return false;
    }
    // The number of the frame (or of its first sample), coded as UTF-8 up to 36 bits.
    size_t at = 4;
    const unsigned char first = p[at];
    int extra = 0;
    uint64_t value = 0;
    if ((first & 0x80) == 0) {
        value = first;
    } else if ((first & 0xE0) == 0xC0) {
        extra = 1;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        extra = 2;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        extra = 3;
        value = first & 0x07;
    } else if ((first & 0xFC) == 0xF8) {
        extra = 4;
        value = first & 0x03;
    } else if ((first & 0xFE) == 0xFC) {
        extra = 5;
        value = first & 0x01;
    } else if (first == 0xFE) {
        extra = 6;
        value = 0;
    } else {
        return false;
    }
    ++at;
    for (int i = 0; i < extra; ++i, ++at) {
        if ((p[at] & 0xC0) != 0x80) {
            return false;
        }
        value = (value << 6) | (p[at] & 0x3F);
    }
    uint64_t block = 0;
    if (blockCode == 1) {
        block = 192;
    } else if (blockCode >= 2 && blockCode <= 5) {
        block = uint64_t(576) << (blockCode - 2);
    } else if (blockCode == 6) {
        block = static_cast<uint64_t>(p[at]) + 1;
        ++at;
    } else if (blockCode == 7) {
        block = ((static_cast<uint64_t>(p[at]) << 8) | p[at + 1]) + 1;
        at += 2;
    } else {
        block = uint64_t(256) << (blockCode - 8);
    }
    at += rateCode == 12 ? 1 : (rateCode == 13 || rateCode == 14 ? 2 : 0);
    if (at + 1 > n || crc8(p, at) != p[at]) {
        return false;
    }
    if (variable) {
        frame.start = value;
    } else {
        if (info.blockSize <= 0) {
            return false;
        }
        frame.start = value * static_cast<uint64_t>(info.blockSize);
    }
    frame.end = frame.start + block;
    return true;
}

} // namespace

std::optional<uint64_t> flacSamplesFromTail(const QString &path, const FlacInfo &info)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const qint64 size = file.size();
    const qint64 tail = std::min<qint64>(size, 4 << 20);
    if (!file.seek(size - tail)) {
        return std::nullopt;
    }
    const QByteArray bytes = file.read(tail);
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
    std::vector<FrameAt> frames;
    for (size_t i = 0; i + 16 <= static_cast<size_t>(bytes.size()); ++i) {
        FrameAt f;
        if (data[i] == 0xFF && frameHeader(data + i, static_cast<size_t>(bytes.size()) - i, info, f)) {
            frames.push_back(f);
        }
    }
    // What is a frame is what has frames that follow it: the last of three in a row, each starting where the one before
    // ends (a chance header in the middle of a frame has no such run).
    for (size_t j = frames.size(); j-- > 2;) {
        if (frames[j - 1].end == frames[j].start && frames[j - 2].end == frames[j - 1].start) {
            return frames[j].end;
        }
    }
    return std::nullopt;
}

std::vector<int16_t> decodeWindow(const QString &path, double headerRate, uint64_t start, size_t count, int audioStream)
{
    // -ss before -i jumps to the mark before the place and the decoder goes on to it sample by sample: the window is exact.
    // A little more than asked for is decoded, and cut.
    const auto seconds = [headerRate](double samples) { return QString::number(samples / headerRate, 'f', 9); };
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(count * 2 + 8));
    streamFfmpegOutput({QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-ss"), seconds(static_cast<double>(start)),
                        QStringLiteral("-i"), path, QStringLiteral("-map"), audioStream >= 0 ? QStringLiteral("0:a:%1").arg(audioStream) : QStringLiteral("0:a:0"),
                        QStringLiteral("-t"), seconds(static_cast<double>(count) + 2.0), QStringLiteral("-f"), QStringLiteral("s16le"), QStringLiteral("-acodec"),
                        QStringLiteral("pcm_s16le"), QStringLiteral("-")},
                       [&bytes](const char *data, size_t n) { bytes.append(data, static_cast<qsizetype>(n)); }, "ffmpeg could not decode " + path.toStdString());
    const size_t got = std::min<size_t>(count, static_cast<size_t>(bytes.size()) / 2);
    std::vector<int16_t> out(got);
    std::memcpy(out.data(), bytes.constData(), got * 2);
    return out;
}

} // namespace rfx
