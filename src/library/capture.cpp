/******************************************************************************
 * capture.cpp
 * rfx library - the capture files given to pack
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "capture.h"

#include "error.h"
#include "ffmpeg.h"
#include "sha256.h"

#include <QCryptographicHash>
#include <QDir>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace rfx {

namespace {

constexpr size_t kByteChunk = size_t(1) << 24;
constexpr size_t kSampleChunk = size_t(1) << 22;
// Largest sidecar kept.
constexpr qint64 kSidecarMax = qint64(1) << 20;

std::string trimmed(const std::string &s)
{
    size_t a = 0;
    size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

bool isCompressed(const QString &path, const std::string &format)
{
    if (format == "ldf" || format == "flac") {
        return true;
    }
    if (format != "auto") {
        return false;
    }
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("ldf") || ext == QLatin1String("flac");
}


} // namespace

QString withExtension(const QString &path, const QString &extension)
{
    const int slash = std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > slash + 1 ? path.left(dot) : path;
    return stem + QLatin1Char('.') + extension;
}

std::string hashBytes(PosFile &file, uint64_t size)
{
    Sha256 hash;
    std::vector<char> buf(kByteChunk);
    uint64_t off = 0;
    while (off < size) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size - off, buf.size()));
        file.readAt(buf.data(), n, off);
        hash.update(buf.data(), n);
        off += n;
    }
    return hash.finishHex();
}

namespace {

// Feed `n` samples to `hash` as little-endian i32 (the hash of the samples, section 3).
void hashSampleBlock(Sha256 &hash, const int32_t *samples, size_t n, std::vector<uint8_t> &scratch)
{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    (void) scratch;
    hash.update(samples, n * 4);
#else
    scratch.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = static_cast<uint32_t>(samples[i]);
        scratch[4 * i] = static_cast<uint8_t>(v);
        scratch[4 * i + 1] = static_cast<uint8_t>(v >> 8);
        scratch[4 * i + 2] = static_cast<uint8_t>(v >> 16);
        scratch[4 * i + 3] = static_cast<uint8_t>(v >> 24);
    }
    hash.update(scratch.data(), scratch.size());
#endif
}

} // namespace

std::string hashSamples(PosFile &file, SampleFormat fmt, uint64_t count)
{
    Sha256 hash;
    std::vector<int32_t> buf(kSampleChunk);
    std::vector<uint8_t> scratch;
    uint64_t at = 0;
    while (at < count) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(count - at, buf.size()));
        loadSamples(fmt, file, count, static_cast<int64_t>(at), buf.data(), n);
        hashSampleBlock(hash, buf.data(), n, scratch);
        at += n;
    }
    return hash.finishHex();
}

CaptureStats captureStats(const QString &path, SampleFormat fmt)
{
    PosFile file(path, PosFile::Mode::Read);
    return captureStats(file, fmt);
}

CaptureStats captureStats(PosFile &file, SampleFormat fmt)
{
    CaptureStats st;
    st.size = file.size();
    const uint64_t words = formatSamples(fmt, st.size);
    st.sha = hashBytes(file, st.size);
    uint32_t orAll = 0;
    int64_t sum = 0;
    // One pass over the samples gives the shift, the mean and the hash of the samples.
    Sha256 sampleHash;
    std::vector<uint8_t> scratch;
    std::vector<int32_t> vals(kSampleChunk);
    for (uint64_t at = 0; at < words;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(words - at, vals.size()));
        loadSamples(fmt, file, words, static_cast<int64_t>(at), vals.data(), n);
        for (size_t i = 0; i < n; ++i) {
            orAll |= static_cast<uint32_t>(vals[i]);
            sum += vals[i];
        }
        hashSampleBlock(sampleHash, vals.data(), n, scratch);
        at += n;
    }
    if (orAll != 0) {
        while (((orAll >> st.shift) & 1u) == 0) {
            ++st.shift;
        }
    }
    st.mean = words == 0 ? 0 : static_cast<int32_t>(std::round(static_cast<double>(sum >> st.shift) / static_cast<double>(words)));
    st.sampleSha = sampleHash.finishHex();
    return st;
}

std::vector<Sidecar> findSidecars(const QString &capture)
{
    const QString path = withExtension(capture, QStringLiteral("yaml"));
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > kSidecarMax) {
        return {};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = f.readAll();
    Sidecar sc;
    sc.name = info.fileName().toStdString();
    sc.sha256 = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().toStdString();
    sc.data.assign(data.begin(), data.end());
    return {sc};
}

std::optional<double> sidecarRateHz(const std::vector<Sidecar> &sidecars)
{
    static const std::string key = "\"sample_rate_hz\":";
    for (const Sidecar &sc : sidecars) {
        const QString text = QString::fromUtf8(reinterpret_cast<const char *>(sc.data.data()), static_cast<qsizetype>(sc.data.size()));
        if (text.toUtf8() != QByteArray(reinterpret_cast<const char *>(sc.data.data()), static_cast<qsizetype>(sc.data.size()))) {
            continue;
        }
        const std::string all = text.toStdString();
        size_t start = 0;
        while (start <= all.size()) {
            size_t end = all.find('\n', start);
            if (end == std::string::npos) {
                end = all.size();
            }
            const std::string line = trimmed(all.substr(start, end - start));
            if (line.compare(0, key.size(), key) == 0) {
                std::string value = trimmed(line.substr(key.size()));
                while (!value.empty() && value.back() == ',') {
                    value.pop_back();
                }
                bool ok = false;
                const double v = QString::fromStdString(value).toDouble(&ok);
                if (ok) {
                    return v;
                }
            }
            start = end + 1;
        }
    }
    return std::nullopt;
}

std::optional<CaptureTimes> captureTimes(const std::vector<Sidecar> &sidecars)
{
    static const QRegularExpression started(QStringLiteral(R"re("started"\s*:\s*"([^"]+)")re"));
    static const QRegularExpression finished(QStringLiteral(R"re("finished"\s*:\s*"([^"]+)")re"));
    static const QRegularExpression duration(QStringLiteral(R"re("duration_seconds"\s*:\s*([0-9]+(?:\.[0-9]+)?))re"));
    for (const Sidecar &sc : sidecars) {
        const QString text = QString::fromUtf8(reinterpret_cast<const char *>(sc.data.data()), static_cast<qsizetype>(sc.data.size()));
        const QRegularExpressionMatch s = started.match(text);
        if (!s.hasMatch()) {
            continue;
        }
        CaptureTimes times;
        times.started = s.captured(1).toStdString();
        const QRegularExpressionMatch f = finished.match(text);
        if (f.hasMatch()) {
            times.finished = f.captured(1).toStdString();
        }
        const QRegularExpressionMatch d = duration.match(text);
        if (d.hasMatch()) {
            times.durationSeconds = d.captured(1).toDouble();
        }
        return times;
    }
    return std::nullopt;
}

SampleFormat inputFormat(const QString &path, const std::string &format)
{
    SampleFormat fmt = SampleFormat::S16;
    if (format == "ldf" || format == "flac") {
        return fmt;
    }
    if (format == "auto") {
        if (!parseFormat(QFileInfo(path).suffix().toLower().toStdString(), fmt)) {
            throw Error("cannot tell the format of " + path.toStdString() + " from its extension: use --format");
        }
        return fmt;
    }
    if (!parseFormat(format, fmt)) {
        throw Error("unknown format " + format);
    }
    return fmt;
}

DecodedInputs decodeCompressed(const std::vector<QString> &given, const QString &out, const std::string &format)
{
    DecodedInputs result;
    if (std::none_of(given.begin(), given.end(), [&](const QString &p) { return isCompressed(p, format); })) {
        result.paths = given;
        return result;
    }
    result.workDir = withExtension(out, QStringLiteral("work"));
    QDir().mkpath(result.workDir);
    for (const QString &p : given) {
        if (!isCompressed(p, format)) {
            result.paths.push_back(p);
            continue;
        }
        const QString target = QDir(result.workDir).filePath(QFileInfo(p).completeBaseName() + QStringLiteral(".s16"));
        std::fprintf(stderr, "decoding %s to raw 16-bit samples\n", p.toStdString().c_str());
        try {
            runFfmpeg({"-v", "error", "-y", "-i", p, "-f", "s16le", "-acodec", "pcm_s16le", target}, "ffmpeg could not decode " + p.toStdString());
        } catch (...) {
            QDir(result.workDir).removeRecursively();
            throw;
        }
        result.paths.push_back(target);
    }
    return result;
}

} // namespace rfx
