/******************************************************************************
 * flacmode.cpp
 * rfx library - the FLAC variant of the archive
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "flacmode.h"

#include "archive.h"
#include "capture.h"
#include "ebml.h"
#include "error.h"
#include "ffmpeg.h"
#include "posfile.h"
#include "yaml.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>

#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>

namespace rfx {

namespace {

constexpr size_t kPeekBytes = 64;
constexpr size_t kChunk = size_t(1) << 22;
constexpr const char *kAttachmentName = "rfx.json";
constexpr int kFlacCompressionLevel = 8;

std::string hexSha256(const std::vector<uint8_t> &data)
{
    return QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(data.data()), static_cast<qsizetype>(data.size())), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

// A real number as Rust prints it with {}: no exponent, no ".0" on a whole number.
std::string plainReal(double v)
{
    return QString::number(v, 'f', QLocale::FloatingPointShortest).toStdString();
}

QString safeFileName(const std::string &name, const char *what)
{
    const QString file = QFileInfo(QString::fromStdString(name)).fileName();
    if (file.isEmpty() || file == QLatin1String(".") || file == QLatin1String("..")) {
        throw Error(std::string("bad ") + what + " name in the archive: " + name);
    }
    return file;
}

// SHA-256 of the whole file.
std::string hashWholeFile(const QString &path)
{
    PosFile f(path, PosFile::Mode::Read);
    return hashBytes(f, f.size());
}

// The samples of a capture as 16-bit little-endian ones in a file (the file
// itself when it is one).
QString raw16(const QString &src, SampleFormat fmt, const QString &work, size_t i)
{
    if (fmt == SampleFormat::S16) {
        return src;
    }
    PosFile from(src, PosFile::Mode::Read);
    const uint64_t words = formatSamples(fmt, from.size());
    const QString dst = QDir(work).filePath(QStringLiteral("%1.s16").arg(static_cast<qulonglong>(i)));
    PosFile out(dst, PosFile::Mode::Write);
    std::vector<int32_t> buf(kChunk);
    std::vector<uint8_t> bytes;
    for (uint64_t at = 0; at < words;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(words - at, buf.size()));
        loadSamples(fmt, from, words, static_cast<int64_t>(at), buf.data(), n);
        bytes.clear();
        for (size_t k = 0; k < n; ++k) {
            int16_t s = 0;
            if (!toI16(fmt, buf[k], s)) {
                throw Error("a sample does not fit 16 bits");
            }
            bytes.push_back(static_cast<uint8_t>(s & 0xFF));
            bytes.push_back(static_cast<uint8_t>((s >> 8) & 0xFF));
        }
        out.writeAt(bytes.data(), bytes.size(), at * 2);
        at += n;
    }
    return dst;
}

// SHA-256 of the 16-bit samples of audio stream `i` of `path`, decoded.
std::string decodeHash(const QString &path, size_t i)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    streamFfmpegOutput({"-v", "error", "-i", path, "-map", QStringLiteral("0:a:%1").arg(static_cast<qulonglong>(i)), "-f", "s16le", "-acodec", "pcm_s16le", "-"},
                       [&hash](const char *data, size_t n) { hash.addData(QByteArrayView(data, static_cast<qsizetype>(n))); },
                       "ffmpeg could not decode stream " + std::to_string(i) + " of " + path.toStdString());
    return hash.result().toHex().toStdString();
}

void writeText(const QString &path, const std::string &text)
{
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || out.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size())) {
        throw Error("cannot write " + path.toStdString());
    }
}

} // namespace

bool isFlacArchive(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray head = f.read(kPeekBytes);
    return head.contains("matroska");
}

std::vector<SourceMeta> readFlacMeta(const QString &path, std::string *codec, std::string *medium)
{
    PosFile file(path, PosFile::Mode::Read);
    const ebml::Index idx = ebml::scan(file, std::nullopt);
    for (const ebml::Attachment &a : idx.attachments) {
        if (a.name == kAttachmentName) {
            std::vector<uint8_t> buf(static_cast<size_t>(a.len));
            file.readAt(buf.data(), buf.size(), a.off);
            return parseFlacMeta(buf.data(), buf.size(), codec, medium);
        }
    }
    throw Error(path.toStdString() + ": no rfx.json attachment (not an rfx archive)");
}

void packFlac(const std::vector<QString> &inputs, const QString &output, const PackOptions &o, StoredCodec codec, const std::vector<StoredTrackEdit> &edits)
{
    const bool rawPcm = codec == StoredCodec::Raw;
    if (inputs.empty()) {
        throw Error("no input");
    }
    const DecodedInputs decoded = decodeCompressed(inputs, output, o.format);
    const QString work = withExtension(output, QStringLiteral("flac-work"));
    QDir().mkpath(work);
    struct Cleanup
    {
        QString work;
        QString converted;
        ~Cleanup()
        {
            QDir(work).removeRecursively();
            if (!converted.isEmpty()) {
                QDir(converted).removeRecursively();
            }
        }
    } cleanup{work, decoded.workDir};

    std::vector<SourceMeta> tracks;
    std::vector<QString> flacs;
    std::vector<std::string> hashes;
    std::vector<std::string> names;
    for (size_t i = 0; i < decoded.paths.size(); ++i) {
        const QString &p = decoded.paths[i];
        const StoredTrackEdit *edit = i < edits.size() ? &edits[i] : nullptr;
        const std::string name = edit && edit->name ? *edit->name : QFileInfo(inputs[i]).fileName().toStdString();
        if (name.empty()) {
            throw Error("input has no file name");
        }
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            throw Error("two captures are named " + name + ": the archive keeps file names only");
        }
        names.push_back(name);
        const SampleFormat fmt = inputFormat(p, o.format);
        std::fprintf(stderr, "%s: hashing and encoding\n", name.c_str());
        const CaptureStats st = captureStats(p, fmt);
        SourceMeta sm;
        sm.name = name;
        sm.size = st.size;
        sm.sha256 = st.sha;
        sm.words = formatSamples(fmt, st.size);
        const uint64_t covered = formatCovered(fmt, st.size);
        sm.tail.resize(static_cast<size_t>(st.size - covered));
        if (!sm.tail.empty()) {
            PosFile f(p, PosFile::Mode::Read);
            f.readAt(sm.tail.data(), sm.tail.size(), covered);
        }
        sm.fmt = formatName(fmt);
        sm.sampleSha256 = st.sampleSha;
        sm.shift = st.shift;
        sm.mean = st.mean;
        sm.position = i;
        sm.sidecars = findSidecars(inputs[i]);
        sm.fileTime = QFileInfo(inputs[i]).lastModified().toUTC().toString(Qt::ISODate).toStdString();
        const std::optional<double> hz = sidecarRateHz(sm.sidecars);
        sm.rateHz = hz ? *hz : o.rateMhz * 1e6;
        if (edit) {
            if (edit->rateHz) {
                sm.rateHz = *edit->rateHz;
            }
            if (edit->notes) {
                sm.notes = *edit->notes;
            }
            if (edit->fileTime) {
                sm.fileTime = *edit->fileTime;
            }
            if (edit->bits) {
                sm.bits = *edit->bits;
            }
            if (edit->sidecars) {
                sm.sidecars = *edit->sidecars;
            }
            sm.apply(edit->info);
        }
        if (inputs[i] != p) {
            sm.origin = QFileInfo(inputs[i]).fileName().toStdString();
        }
        const QString raw = raw16(p, fmt, work, i);
        hashes.push_back(hashWholeFile(raw));
        const QString flac = QDir(work).filePath(QStringLiteral("%1.%2").arg(static_cast<qulonglong>(i)).arg(rawPcm ? QStringLiteral("mka") : QStringLiteral("flac")));
        QStringList encode = {"-v", "error", "-y", "-f", "s16le", "-ar", QString::number(static_cast<qulonglong>(std::llround(sm.rateHz / 1000.0))), "-ac", "1", "-i", raw};
        if (rawPcm) {
            encode << "-c:a" << "pcm_s16le" << flac;
        } else {
            encode << "-c:a" << "flac" << "-compression_level" << QString::number(kFlacCompressionLevel) << flac;
        }
        runFfmpeg(encode, "ffmpeg could not encode " + name);
        if (raw != p) {
            QFile::remove(raw);
        }
        flacs.push_back(flac);
        tracks.push_back(std::move(sm));
    }
    const QString json = QDir(work).filePath(QStringLiteral("rfx.json"));
    const std::vector<uint8_t> metaBytes = serializeFlacMeta(tracks, rawPcm ? "raw" : "flac", o.medium.value_or(std::string()));
    writeText(json, std::string(metaBytes.begin(), metaBytes.end()));
    const QString tmp = withExtension(output, QStringLiteral("rfx-tmp"));
    QStringList args = {"-v", "error", "-y"};
    for (const QString &f : flacs) {
        args << "-i" << f;
    }
    for (size_t i = 0; i < flacs.size(); ++i) {
        args << "-map" << QString::number(static_cast<qulonglong>(i));
    }
    args << "-c" << "copy";
    for (size_t i = 0; i < tracks.size(); ++i) {
        args << QStringLiteral("-metadata:s:a:%1").arg(static_cast<qulonglong>(i)) << QStringLiteral("title=%1").arg(QString::fromStdString(tracks[i].name));
    }
    args << "-attach" << json << "-metadata:s:t:0" << "mimetype=application/json" << "-metadata:s:t:0" << "filename=rfx.json" << "-f" << "matroska" << tmp;
    try {
        runFfmpeg(args, "ffmpeg could not write " + output.toStdString());
        if (o.verify) {
            for (size_t i = 0; i < hashes.size(); ++i) {
                if (decodeHash(tmp, i) != hashes[i]) {
                    throw Error(tracks[i].name + ": the samples differ after encoding with ffmpeg");
                }
            }
        }
    } catch (...) {
        QFile::remove(tmp);
        throw;
    }
    const uint64_t total = static_cast<uint64_t>(QFileInfo(tmp).size());
    uint64_t raw = 0;
    for (const SourceMeta &t : tracks) {
        raw += t.size;
    }
    if (QFile::exists(output) && !QFile::remove(output)) {
        throw Error("cannot replace " + output.toStdString());
    }
    if (!QFile::rename(tmp, output)) {
        throw Error("writing " + output.toStdString());
    }
    std::fprintf(stderr, "total: %llu -> %llu bytes (%.1f%%), %s tracks\n", static_cast<unsigned long long>(raw),
                 static_cast<unsigned long long>(total), 100.0 * static_cast<double>(total) / static_cast<double>(std::max<uint64_t>(raw, 1)), rawPcm ? "raw PCM" : "FLAC");
}

std::string flacInfo(const QString &path)
{
    std::string codec;
    std::string medium;
    const std::vector<SourceMeta> tracks = readFlacMeta(path, &codec, &medium);
    std::string out = QString::asprintf("%s: %zu tracks, %s\n", path.toStdString().c_str(), tracks.size(), codec == "raw" ? "raw PCM" : "FLAC").toStdString();
    out += "  source: " + mediumName(medium) + "\n";
    for (size_t k = 0; k < tracks.size(); ++k) {
        const SourceMeta &sm = tracks[k];
        out += QString::asprintf("  %zu: %s  %llu bytes  %llu samples at %s Hz  format %s\n", k, sm.name.c_str(), static_cast<unsigned long long>(sm.size),
                                 static_cast<unsigned long long>(sm.words), plainReal(sm.rateHz).c_str(), sm.fmt.c_str())
                   .toStdString();
        out += "     sha256 " + sm.sha256 + "\n";
        std::string side = "[";
        for (size_t i = 0; i < sm.sidecars.size(); ++i) {
            side += (i ? ", " : "") + jsonQuote(sm.sidecars[i].name);
        }
        out += QString::asprintf("     on the command line: %zu  sidecars %s]\n", sm.position, side.c_str()).toStdString();
        if (sm.hasInfo()) {
            out += "     disc: " + describeInfo(sm) + "\n";
        }
    }
    return out;
}

std::string flacYaml(const QString &path)
{
    std::string codec;
    std::string medium;
    const std::vector<SourceMeta> tracks = readFlacMeta(path, &codec, &medium);
    Node list = Node::array();
    for (size_t k = 0; k < tracks.size(); ++k) {
        const SourceMeta &sm = tracks[k];
        Node t = Node::object();
        t.set("name", Node::string(sm.name));
        t.set("size", Node::unsignedInteger(sm.size));
        t.set("sha256", Node::string(sm.sha256));
        t.set("words", Node::unsignedInteger(sm.words));
        t.set("tail", Node::string(toHex(sm.tail.data(), sm.tail.size())));
        t.set("fmt", Node::string(sm.fmt));
        t.set("sample_sha256", Node::string(sm.sampleSha256));
        t.set("shift", Node::unsignedInteger(sm.shift));
        t.set("mean", Node::integer(sm.mean));
        t.set("position", Node::unsignedInteger(sm.position));
        t.set("origin", sm.origin ? Node::string(*sm.origin) : Node());
        t.set("rate_hz", Node::real(sm.rateHz));
        t.set("track", Node::unsignedInteger(k));
        Node sidecars = Node::array();
        for (const Sidecar &sc : sm.sidecars) {
            Node e = Node::object();
            e.set("name", Node::string(sc.name));
            e.set("sha256", Node::string(sc.sha256));
            e.set("bytes", Node::unsignedInteger(sc.data.size()));
            const QByteArray raw(reinterpret_cast<const char *>(sc.data.data()), static_cast<qsizetype>(sc.data.size()));
            const QString text = QString::fromUtf8(raw);
            if (text.toUtf8() == raw) {
                e.set("text", Node::string(text.toStdString()));
            }
            sidecars.push(e);
        }
        if (!sm.standard.empty()) {
            t.set("standard", Node::string(sm.standard));
        }
        if (!sm.reference.empty()) {
            t.set("reference", Node::string(sm.reference));
        }
        if (sm.copy > 0) {
            t.set("copy", Node::unsignedInteger(sm.copy));
        }
        if (sm.capture > 0) {
            t.set("capture", Node::unsignedInteger(sm.capture));
        }
        if (!sm.face.empty()) {
            t.set("face", Node::string(sm.face));
        }
        if (!sm.signal.empty()) {
            t.set("signal", Node::string(sm.signal));
        }
        t.set("sidecars", sidecars);
        list.push(t);
    }
    Node doc = Node::object();
    doc.set("format", Node::string(kFormatName));
    doc.set("codec", Node::string(codec));
    doc.set("version", Node::unsignedInteger(kFormatVersion));
    if (!medium.empty()) {
        doc.set("medium", Node::string(medium));
    }
    doc.set("tracks", list);
    std::string out;
    doc.writeYaml(0, out);
    return out;
}

void unpackFlacMetadata(const QString &path, const QString &outdir, std::optional<size_t> only)
{
    const std::vector<SourceMeta> tracks = readFlacMeta(path);
    if (!QDir().mkpath(outdir)) {
        throw Error("cannot create " + outdir.toStdString());
    }
    const QDir dir(outdir);
    for (size_t k = 0; k < tracks.size(); ++k) {
        if (only && *only != k) {
            continue;
        }
        for (const Sidecar &sc : tracks[k].sidecars) {
            if (hexSha256(sc.data) != sc.sha256) {
                throw Error(tracks[k].name + ": the stored " + sc.name + " does not match its hash");
            }
            const QString target = dir.filePath(safeFileName(sc.name, "sidecar"));
            writeText(target, std::string(sc.data.begin(), sc.data.end()));
            std::printf("%s\n", QDir::toNativeSeparators(target).toStdString().c_str());
        }
    }
    const QString target = dir.filePath(QFileInfo(path).fileName() + QStringLiteral(".yaml"));
    writeText(target, flacYaml(path));
    std::printf("%s\n", QDir::toNativeSeparators(target).toStdString().c_str());
}

void unpackFlac(const QString &path, const QString &outdir, std::optional<size_t> only, const std::optional<OutputFormat> &target)
{
    const std::vector<SourceMeta> tracks = readFlacMeta(path);
    if (!QDir().mkpath(outdir)) {
        throw Error("cannot create " + outdir.toStdString());
    }
    const QDir dir(outdir);
    for (size_t k = 0; k < tracks.size(); ++k) {
        if (only && *only != k) {
            continue;
        }
        const SourceMeta &sm = tracks[k];
        const SampleFormat orig = sm.format();
        const QString dst = dir.filePath(safeFileName(sm.name, "track"));
        {
            PosFile out(dst, PosFile::Mode::Write);
            out.setSize(sm.size);
            std::vector<char> held;
            uint64_t at = 0;
            auto flush = [&](bool last) {
                const size_t whole = held.size() / 2;
                if (whole == 0 && !last) {
                    return;
                }
                std::vector<int32_t> vals(whole);
                for (size_t i = 0; i < whole; ++i) {
                    const int16_t s = static_cast<int16_t>(static_cast<uint8_t>(held[2 * i]) | (static_cast<uint8_t>(held[2 * i + 1]) << 8));
                    vals[i] = fromI16(orig, s);
                }
                if (at + vals.size() > sm.words) {
                    throw Error(sm.name + ": more samples than were packed");
                }
                if (!vals.empty()) {
                    storeSamples(orig, out, at, vals.data(), vals.size());
                }
                at += whole;
                held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(2 * whole));
            };
            streamFfmpegOutput({"-v", "error", "-i", path, "-map", QStringLiteral("0:a:%1").arg(static_cast<qulonglong>(k)), "-f", "s16le", "-acodec", "pcm_s16le", "-"},
                               [&](const char *data, size_t n) {
                                   held.insert(held.end(), data, data + n);
                                   if (held.size() >= kChunk) {
                                       flush(false);
                                   }
                               },
                               "ffmpeg could not decode " + sm.name);
            flush(true);
            if (at != sm.words) {
                throw Error(sm.name + ": " + std::to_string(at) + " samples decoded, " + std::to_string(sm.words) + " packed");
            }
            if (!sm.tail.empty()) {
                out.writeAt(sm.tail.data(), sm.tail.size(), formatCovered(orig, sm.size));
            }
        }
        const std::string got = hashWholeFile(dst);
        if (got != sm.sha256) {
            throw Error(sm.name + ": SHA-256 differs from the original (" + got + " against " + sm.sha256 + ")");
        }
        std::fprintf(stderr, "%s: %llu bytes, SHA-256 matches\n", sm.name.c_str(), static_cast<unsigned long long>(sm.size));
        for (const Sidecar &sc : sm.sidecars) {
            writeText(dir.filePath(safeFileName(sc.name, "sidecar")), std::string(sc.data.begin(), sc.data.end()));
        }
        if (target && target->kind == OutputFormat::Kind::Raw && target->raw != orig) {
            convertFile(dst, sm, target->raw, outdir);
        } else if (target && (target->kind == OutputFormat::Kind::Flac || target->kind == OutputFormat::Kind::Ldf)) {
            encodeFlacFile(dst, sm, target->kind == OutputFormat::Kind::Ldf, outdir);
        }
    }
}

void assembleStored(const std::vector<QString> &archives, const std::vector<Pick> &picks, const QString &output, const PackOptions &o, StoredCodec codec)
{
    if (picks.empty()) {
        throw Error("nothing to write: no track selected");
    }
    struct Source
    {
        bool stored = false;
        std::string codecName;
        std::string medium;
        std::vector<SourceMeta> tracks;
    };
    std::vector<Source> sources;
    for (const QString &a : archives) {
        Source s;
        s.stored = isFlacArchive(a);
        if (s.stored) {
            s.tracks = readFlacMeta(a, &s.codecName, &s.medium);
        } else {
            const Archive opened(a);
            s.tracks = opened.meta().sources;
            s.medium = opened.meta().medium;
        }
        sources.push_back(std::move(s));
    }
    // What was captured, of the file: what is asked, or what the files the tracks come from say (when they agree).
    std::string medium;
    if (o.medium) {
        medium = *o.medium;
    } else {
        std::vector<std::string> said;
        std::set<size_t> seen;
        for (const Pick &p : picks) {
            if (p.fromArchive && p.arch < sources.size() && seen.insert(p.arch).second) {
                said.push_back(sources[p.arch].medium);
            }
        }
        medium = mergedMedium(said);
    }
    // Tracks of files of the variant that is asked for, and nothing else: the streams are copied as they are (no decoding, no encoding), and
    // only the description of the tracks (rfx.json) is written again, with what the picks change (name, note, files, bits).
    {
        const char *const wanted = codec == StoredCodec::Raw ? "raw" : "flac";
        bool copyable = true;
        for (const Pick &p : picks) {
            if (!p.fromArchive || p.arch >= sources.size() || !sources[p.arch].stored || sources[p.arch].codecName != wanted) {
                copyable = false;
                break;
            }
            // (a rate that is told otherwise is in the header of the stream: that one is made by encoding)
            if (p.src < sources[p.arch].tracks.size() && p.edit.rateHz && std::llround(*p.edit.rateHz / 1000.0) != std::llround(sources[p.arch].tracks[p.src].rateHz / 1000.0)) {
                copyable = false;
                break;
            }
        }
        if (copyable) {
            std::vector<SourceMeta> tracks;
            std::vector<size_t> inputOf(sources.size(), static_cast<size_t>(-1));
            std::vector<size_t> used;
            QStringList args = {"-v", "error", "-y"};
            for (size_t k = 0; k < picks.size(); ++k) {
                const Pick &p = picks[k];
                if (p.src >= sources[p.arch].tracks.size()) {
                    throw Error("no track " + std::to_string(p.src) + " in archive " + std::to_string(p.arch));
                }
                SourceMeta m = sources[p.arch].tracks[p.src];
                if (p.edit.name) {
                    m.name = *p.edit.name;
                }
                if (p.edit.notes) {
                    m.notes = *p.edit.notes;
                }
                if (p.edit.bits) {
                    m.bits = *p.edit.bits;
                }
                if (p.edit.sidecars) {
                    m.sidecars = *p.edit.sidecars;
                }
                m.apply(p.edit.info);
                m.position = k;
                for (const SourceMeta &other : tracks) {
                    if (other.name == m.name) {
                        throw Error("two tracks are named " + m.name + ": the archive keeps file names only");
                    }
                }
                tracks.push_back(std::move(m));
                if (inputOf[p.arch] == static_cast<size_t>(-1)) {
                    inputOf[p.arch] = used.size();
                    used.push_back(p.arch);
                    args << "-i" << archives[p.arch];
                }
            }
            for (size_t k = 0; k < picks.size(); ++k) {
                args << "-map" << QStringLiteral("%1:a:%2").arg(static_cast<qulonglong>(inputOf[picks[k].arch])).arg(static_cast<qulonglong>(picks[k].src));
            }
            args << "-c" << "copy" << "-map_metadata" << "-1";
            for (size_t i = 0; i < tracks.size(); ++i) {
                args << QStringLiteral("-metadata:s:a:%1").arg(static_cast<qulonglong>(i)) << QStringLiteral("title=%1").arg(QString::fromStdString(tracks[i].name));
            }
            const QString tmp = withExtension(output, QStringLiteral("rfx-tmp"));
            const QString dir = withExtension(output, QStringLiteral("stored-work"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            struct Remove
            {
                QString dir;
                ~Remove() { QDir(dir).removeRecursively(); }
            } remove{dir};
            const QString json = QDir(dir).filePath(QStringLiteral("rfx.json"));
            const std::vector<uint8_t> metaBytes = serializeFlacMeta(tracks, wanted, medium);
            writeText(json, std::string(metaBytes.begin(), metaBytes.end()));
            args << "-attach" << json << "-metadata:s:t:0" << "mimetype=application/json" << "-metadata:s:t:0" << "filename=rfx.json" << "-f" << "matroska" << tmp;
            std::fprintf(stderr, "%zu track(s) copied as they are, no decoding and no encoding\n", tracks.size());
            try {
                runFfmpeg(args, "ffmpeg could not write " + output.toStdString());
            } catch (...) {
                QFile::remove(tmp);
                throw;
            }
            if (QFile::exists(output) && !QFile::remove(output)) {
                QFile::remove(tmp);
                throw Error("cannot replace " + output.toStdString());
            }
            if (!QFile::rename(tmp, output)) {
                throw Error("writing " + output.toStdString());
            }
            return;
        }
    }
    const QString work = withExtension(output, QStringLiteral("stored-work"));
    QDir(work).removeRecursively();
    QDir().mkpath(work);
    struct Cleanup
    {
        QString work;
        ~Cleanup() { QDir(work).removeRecursively(); }
    } cleanup{work};

    std::vector<QString> files;
    std::vector<StoredTrackEdit> edits;
    std::string format;
    bool same = true;
    for (size_t k = 0; k < picks.size(); ++k) {
        const Pick &p = picks[k];
        StoredTrackEdit e;
        if (!p.fromArchive) {
            files.push_back(p.capture);
            e.rateHz = p.edit.rateHz;
            e.notes = p.edit.notes;
            e.bits = p.edit.bits;
            e.info = p.edit.info;
            edits.push_back(std::move(e));
            same = false;
            continue;
        }
        if (p.arch >= sources.size() || p.src >= sources[p.arch].tracks.size()) {
            throw Error("no track " + std::to_string(p.src) + " in archive " + std::to_string(p.arch));
        }
        const SourceMeta &sm = sources[p.arch].tracks[p.src];
        const QString dir = QDir(work).filePath(QString::number(k));
        QDir().mkpath(dir);
        if (sources[p.arch].stored) {
            unpackFlac(archives[p.arch], dir, p.src, std::nullopt);
        } else {
            UnpackOptions unpack;
            unpack.only = p.src;
            unpack.device = o.device;
            unpack.threads = o.threads;
            unpackArchive(archives[p.arch], dir, unpack);
        }
        files.push_back(QDir(dir).filePath(safeFileName(sm.name, "track")));
        e.name = p.edit.name ? p.edit.name : std::optional<std::string>(sm.name);
        if (p.edit.rateHz) {
            e.rateHz = p.edit.rateHz;
        } else if (sm.rateHz > 0) {
            e.rateHz = sm.rateHz;
        }
        e.notes = p.edit.notes ? p.edit.notes : std::optional<std::string>(sm.notes);
        e.fileTime = sm.fileTime;
        if (p.edit.bits) {
            e.bits = p.edit.bits;
        } else if (sm.bits > 0) {
            e.bits = sm.bits;
        }
        e.sidecars = p.edit.sidecars ? p.edit.sidecars : std::optional<std::vector<Sidecar>>(sm.sidecars);
        e.info = InfoEdit::of(sm);
        e.info.overlay(p.edit.info);
        edits.push_back(std::move(e));
        if (format.empty()) {
            format = sm.fmt;
        } else if (format != sm.fmt) {
            same = false;
        }
    }
    PackOptions options = o;
    options.medium = medium;
    options.order = OrderMode::Given;
    // The tracks come out under the names they had, whatever their extensions: the format is given, when it is one for all.
    options.format = same && !format.empty() ? format : "auto";
    packFlac(files, output, options, codec, edits);
}

} // namespace rfx
