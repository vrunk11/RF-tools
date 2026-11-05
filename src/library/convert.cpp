/******************************************************************************
 * convert.cpp
 * rfx library - giving the captures back in another format
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "convert.h"

#include "archive.h"
#include "capture.h"
#include "error.h"
#include "ffmpeg.h"
#include "flacmode.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rfx {

namespace {

// Frames (samples of every channel) read at a time.
constexpr size_t kFrames = size_t(1) << 20;
constexpr size_t kSampleChunk = size_t(1) << 22;
// FLAC's most channels in a file.
constexpr size_t kFlacChannels = 8;
constexpr const char *kUnknownRate = "40k";
constexpr const char *kUnknownRateKhz = "40000";

QString rateArgument(double rateHz, const char *unknown)
{
    return rateHz > 0 ? QString::number(static_cast<qulonglong>(std::llround(rateHz / 1000.0))) : QString::fromLatin1(unknown);
}

QString nameWithExtension(const std::string &name, const QString &extension)
{
    return withExtension(QString::fromStdString(name), extension);
}

} // namespace

bool parseOutputFormat(const std::string &name, OutputFormat &out)
{
    if (name == "flac") {
        out.kind = OutputFormat::Kind::Flac;
    } else if (name == "ldf") {
        out.kind = OutputFormat::Kind::Ldf;
    } else if (name == "mflac") {
        out.kind = OutputFormat::Kind::MultichannelFlac;
    } else if (parseFormat(name, out.raw)) {
        out.kind = OutputFormat::Kind::Raw;
    } else {
        return false;
    }
    return true;
}

bool toI16(SampleFormat fmt, int32_t v, int16_t &out)
{
    int32_t x = v;
    switch (fmt) {
    case SampleFormat::S8: x = v * 256; break;
    case SampleFormat::U8: x = (v - 128) * 256; break;
    case SampleFormat::U16: x = v - 32768; break;
    case SampleFormat::S16:
    case SampleFormat::Lds: break;
    }
    if (x < -32768 || x > 32767) {
        return false;
    }
    out = static_cast<int16_t>(x);
    return true;
}

int32_t fromI16(SampleFormat fmt, int16_t s)
{
    const int32_t v = s;
    switch (fmt) {
    case SampleFormat::S8: return v / 256;
    case SampleFormat::U8: return v / 256 + 128;
    case SampleFormat::U16: return v + 32768;
    case SampleFormat::S16:
    case SampleFormat::Lds: break;
    }
    return v;
}

void convertFile(const QString &src, const SourceMeta &sm, SampleFormat target, const QString &outdir)
{
    const QString dstPath = QDir(outdir).filePath(QFileInfo(nameWithExtension(sm.name, QString::fromLatin1(formatName(target)))).fileName());
    {
        PosFile from(src, PosFile::Mode::Read);
        PosFile dst(dstPath, PosFile::Mode::Write);
        std::vector<int32_t> buf(kSampleChunk);
        for (uint64_t at = 0; at < sm.words;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(sm.words - at, buf.size()));
            loadSamples(sm.format(), from, sm.words, static_cast<int64_t>(at), buf.data(), n);
            try {
                storeSamples(target, dst, at, buf.data(), n);
            } catch (const Error &e) {
                throw Error(sm.name + ": converting to " + formatName(target) + ": " + e.what());
            }
            at += n;
        }
        if (hashSamples(dst, target, sm.words) != sm.sampleSha256) {
            throw Error(sm.name + ": the samples differ after conversion to " + formatName(target));
        }
    }
    QFile::remove(src);
    std::fprintf(stderr, "%s -> %s: samples identical\n", sm.name.c_str(), QFileInfo(dstPath).fileName().toStdString().c_str());
}

void encodeFlacFile(const QString &src, const SourceMeta &sm, bool ogg, const QString &outdir)
{
    // ffmpeg reads 16-bit samples: a capture in another format goes through a raw copy.
    QString raw = src;
    if (sm.format() != SampleFormat::S16) {
        raw = QDir(outdir).filePath(QString::fromStdString(sm.name) + QStringLiteral(".rfx-tmp.s16"));
        PosFile from(src, PosFile::Mode::Read);
        PosFile dst(raw, PosFile::Mode::Write);
        std::vector<int32_t> buf(kSampleChunk);
        for (uint64_t at = 0; at < sm.words;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(sm.words - at, buf.size()));
            loadSamples(sm.format(), from, sm.words, static_cast<int64_t>(at), buf.data(), n);
            try {
                storeSamples(SampleFormat::S16, dst, at, buf.data(), n);
            } catch (const Error &) {
                throw Error(sm.name + ": samples do not fit 16 bits");
            }
            at += n;
        }
    }
    const QString dstPath = QDir(outdir).filePath(QFileInfo(QString::fromStdString(sm.name)).completeBaseName() + (ogg ? QStringLiteral(".ldf") : QStringLiteral(".flac")));
    // The container rate is the RF rate in kHz (75 MHz -> 75000 Hz), as ld-decode reads it.
    QStringList args = {"-v", "error", "-y", "-f", "s16le", "-ar", rateArgument(sm.rateHz, kUnknownRate), "-ac", "1", "-i", raw, "-c:a", "flac"};
    if (ogg) {
        args << "-f" << "ogg";
    }
    args << dstPath;
    runFfmpeg(args, "ffmpeg could not encode " + sm.name);
    // Decode it back and compare.
    const QString back = QDir(outdir).filePath(QString::fromStdString(sm.name) + QStringLiteral(".rfx-check.s16"));
    bool ok = false;
    try {
        runFfmpeg({"-v", "error", "-y", "-i", dstPath, "-f", "s16le", "-acodec", "pcm_s16le", back}, "ffmpeg could not decode " + sm.name);
        PosFile check(back, PosFile::Mode::Read);
        ok = hashSamples(check, SampleFormat::S16, sm.words) == sm.sampleSha256;
    } catch (const Error &) {
        ok = false;
    }
    QFile::remove(back);
    if (raw != src) {
        QFile::remove(raw);
    }
    if (!ok) {
        throw Error(sm.name + ": the samples differ after encoding with ffmpeg");
    }
    QFile::remove(src);
    std::fprintf(stderr, "%s -> %s: samples identical\n", sm.name.c_str(), QFileInfo(dstPath).fileName().toStdString().c_str());
}

void exportMultichannelFlac(const QString &archivePath, const QString &outdir, std::optional<size_t> only)
{
    // A file of the predictor variant, or of the FLAC or raw one.
    const bool stored = isFlacArchive(archivePath);
    const std::vector<SourceMeta> metas = stored ? readFlacMeta(archivePath) : Archive(archivePath).meta().sources;
    std::vector<size_t> tracks;
    {
        const size_t n = metas.size();
        if (only && *only >= n) {
            throw Error(archivePath.toStdString() + " has " + std::to_string(n) + " tracks: no track " + std::to_string(*only));
        }
        for (size_t k = 0; k < n; ++k) {
            if (!only || *only == k) {
                tracks.push_back(k);
            }
        }
    }
    if (!QDir().mkpath(outdir)) {
        throw Error("cannot create " + outdir.toStdString());
    }
    const QString stem = QFileInfo(archivePath).completeBaseName();
    // The samples come from the tracks decoded to a work directory.
    const QString workPath = QDir(outdir).filePath(stem + QStringLiteral(".mflac-work"));
    struct RemoveWork
    {
        QString path;
        ~RemoveWork() { QDir(path).removeRecursively(); }
    } cleanup{workPath};
    if (stored) {
        unpackFlac(archivePath, workPath, std::nullopt, std::nullopt);
    } else {
        unpackArchive(archivePath, workPath, UnpackOptions());
    }
    std::vector<std::unique_ptr<PosFile>> files;
    for (const SourceMeta &sm : metas) {
        files.push_back(std::make_unique<PosFile>(QDir(workPath).filePath(QFileInfo(QString::fromStdString(sm.name)).fileName()), PosFile::Mode::Read));
    }
    const size_t groups = (tracks.size() + kFlacChannels - 1) / kFlacChannels;
    for (size_t g = 0; g < groups; ++g) {
        const std::vector<size_t> group(tracks.begin() + static_cast<std::ptrdiff_t>(g * kFlacChannels),
                                        tracks.begin() + static_cast<std::ptrdiff_t>(std::min(tracks.size(), (g + 1) * kFlacChannels)));
        const QString name = groups == 1 && !only
            ? stem + QStringLiteral(".flac")
            : QStringLiteral("%1_%2-%3.flac").arg(stem).arg(static_cast<qulonglong>(group.front())).arg(static_cast<qulonglong>(group.back()));
        const QString dst = QDir(outdir).filePath(name);
        const double rate = metas[group.front()].rateHz;
        const QString rateArg = rateArgument(rate, kUnknownRateKhz);
        for (const size_t k : group) {
            if (std::fabs(metas[k].rateHz - rate) > 1.0) {
                std::fprintf(stderr, "%s: the tracks were not captured at one rate; the file says %s Hz (track %zu)\n",
                             name.toStdString().c_str(), rateArg.toStdString().c_str(), group.front());
                break;
            }
        }
        QProcess enc;
        enc.setProcessChannelMode(QProcess::ForwardedChannels);
        enc.start(ffmpegProgram(), {"-v", "error", "-y", "-f", "s16le", "-ar", rateArg, "-ac", QString::number(group.size()), "-i", "-", "-c:a", "flac", dst});
        if (!enc.waitForStarted()) {
            throw Error("starting " + ffmpegProgram().toStdString());
        }
        uint64_t frames = 0;
        for (const size_t k : group) {
            frames = std::max(frames, metas[k].words);
        }
        QCryptographicHash sent(QCryptographicHash::Sha256);
        std::vector<std::vector<int32_t>> bufs(group.size(), std::vector<int32_t>(kFrames));
        std::vector<char> bytes;
        for (uint64_t at = 0; at < frames;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(frames - at, kFrames));
            for (size_t c = 0; c < group.size(); ++c) {
                const SourceMeta &sm = metas[group[c]];
                loadSamples(sm.format(), *files[group[c]], sm.words, static_cast<int64_t>(at), bufs[c].data(), n);
            }
            bytes.clear();
            for (size_t i = 0; i < n; ++i) {
                for (size_t c = 0; c < group.size(); ++c) {
                    const SourceMeta &sm = metas[group[c]];
                    int16_t s = 0;
                    if (at + i < sm.words && !toI16(sm.format(), bufs[c][i], s)) {
                        throw Error("track " + std::to_string(group[c]) + ": a sample does not fit 16 bits");
                    }
                    bytes.push_back(static_cast<char>(s & 0xFF));
                    bytes.push_back(static_cast<char>((s >> 8) & 0xFF));
                }
            }
            sent.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
            qint64 written = 0;
            while (written < static_cast<qint64>(bytes.size())) {
                const qint64 put = enc.write(bytes.data() + written, static_cast<qint64>(bytes.size()) - written);
                if (put < 0) {
                    throw Error("ffmpeg stopped reading");
                }
                written += put;
                if (!enc.waitForBytesWritten(-1)) {
                    throw Error("ffmpeg stopped reading");
                }
            }
            at += n;
        }
        enc.closeWriteChannel();
        enc.waitForFinished(-1);
        if (enc.exitStatus() != QProcess::NormalExit || enc.exitCode() != 0) {
            throw Error("ffmpeg could not encode " + name.toStdString());
        }
        // Decode it back and compare.
        QCryptographicHash back(QCryptographicHash::Sha256);
        streamFfmpegOutput({"-v", "error", "-i", dst, "-f", "s16le", "-acodec", "pcm_s16le", "-"},
                           [&back](const char *data, size_t n) { back.addData(QByteArrayView(data, static_cast<qsizetype>(n))); },
                           "ffmpeg could not decode " + name.toStdString());
        if (back.result() != sent.result()) {
            throw Error(name.toStdString() + ": the samples differ after encoding with ffmpeg");
        }
        std::string list;
        for (const size_t k : group) {
            list += (list.empty() ? "" : ", ") + std::to_string(k) + ": " + metas[k].name;
        }
        std::printf("%s  %s Hz, %zu channels, %llu frames  [%s]\n", QDir::toNativeSeparators(dst).toStdString().c_str(),
                    rateArg.toStdString().c_str(), group.size(), static_cast<unsigned long long>(frames), list.c_str());
    }
}

} // namespace rfx
