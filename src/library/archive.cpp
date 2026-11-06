/******************************************************************************
 * archive.cpp
 * rfx library - reading an .rfx archive and unpacking it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "archive.h"

#include "capture.h"
#include "ebml.h"
#include "gpupipeline.h"
#include "error.h"
#include "parallel.h"
#include "refwindow.h"
#include "segment.h"
#include "yaml.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>

namespace rfx {

namespace {


template<typename... Args>
std::string format(const char *pattern, Args... args)
{
    return QString::asprintf(pattern, args...).toStdString();
}


std::string hexSha256(const std::vector<uint8_t> &data)
{
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char *>(data.data()), static_cast<qsizetype>(data.size())), QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}


// The file name part of a name from the archive, which is data from outside:
// no directory, and not a name that means one.
QString safeName(const std::string &name, const char *what)
{
    const QString file = QFileInfo(QString::fromStdString(name)).fileName();
    if (file.isEmpty() || file == QLatin1String(".") || file == QLatin1String("..")) {
        throw Error(std::string("bad ") + what + " name in the archive: " + name);
    }
    return file;
}

std::string listOfNumbers(const std::vector<size_t> &v)
{
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        out += (i ? ", " : "") + std::to_string(v[i]);
    }
    return out + "]";
}

std::string bitsPerSample(uint64_t packed, uint64_t words)
{
    return format("%.3f", static_cast<double>(packed) * 8.0 / static_cast<double>(std::max<uint64_t>(words, 1)));
}

// The timestamp of the cluster of the metadata track that holds the TBC curves (0 is the metadata).
constexpr uint64_t kSpeedTimestamp = 1;

} // namespace

Archive::Archive(const QString &path)
{
    m_file = std::make_unique<PosFile>(path, PosFile::Mode::Read);
    const ebml::Index idx = ebml::scan(*m_file, std::nullopt);
    if (idx.docType == "matroska") {
        throw Error(path.toStdString() + " holds FLAC tracks (packed with --flac): the C++ tools do not read that variant yet");
    }
    if (idx.docType != "rfx") {
        throw Error("not an rfx file (DocType is not rfx)");
    }
    auto trackOf = [&idx](const char *codec) -> std::optional<uint64_t> {
        for (const auto &c : idx.codecs) {
            if (c.second == codec) {
                return c.first;
            }
        }
        return std::nullopt;
    };
    const std::optional<uint64_t> metaTrack = trackOf(kCodecMeta);
    const std::optional<uint64_t> mapTrack = trackOf(kCodecMap);
    if (!metaTrack) {
        throw Error("no metadata track");
    }
    if (!mapTrack) {
        throw Error("no map track");
    }
    const auto metaBlock = std::find_if(idx.blocks.begin(), idx.blocks.end(), [&](const ebml::BlockRef &b) { return b.track == *metaTrack; });
    if (metaBlock == idx.blocks.end()) {
        throw Error("no metadata block");
    }
    std::vector<uint8_t> buf(static_cast<size_t>(metaBlock->len));
    m_file->readAt(buf.data(), buf.size(), metaBlock->off);
    m_meta = parseMeta(buf.data(), buf.size());

    for (const ebml::BlockRef &b : idx.blocks) {
        if (b.track != *mapTrack) {
            continue;
        }
        std::vector<uint8_t> table(static_cast<size_t>(b.len));
        m_file->readAt(table.data(), table.size(), b.off);
        size_t at = 0;
        const size_t k = static_cast<size_t>(getVarint(table.data(), table.size(), at));
        const size_t j = static_cast<size_t>(getVarint(table.data(), table.size(), at));
        if (j >= k || k >= m_meta.sources.size()) {
            throw Error("a lag table names tracks that are not valid");
        }
        m_tables[{k, j}] = LagTable::decode(table.data() + at, table.size() - at);
    }
    // The TBC curves (specification 3.1): the last block of the metadata track after the first one.
    {
        const ebml::BlockRef *speed = nullptr;
        for (const ebml::BlockRef &b : idx.blocks) {
            if (b.track == *metaTrack && b.ts == kSpeedTimestamp) {
                speed = &b;
            }
        }
        if (speed) {
            std::vector<uint8_t> data(static_cast<size_t>(speed->len));
            m_file->readAt(data.data(), data.size(), speed->off);
            const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data.data()), static_cast<qsizetype>(data.size())));
            for (const QJsonValue &entry : doc.object().value(QLatin1String("pass0")).toArray()) {
                const QJsonObject o = entry.toObject();
                const int track = o.value(QLatin1String("track")).toInt(-1);
                if (track < 0 || static_cast<size_t>(track) >= m_meta.sources.size()) {
                    continue;
                }
                SavedPass0 saved;
                saved.pal = o.value(QLatin1String("system")).toString() == QLatin1String("pal");
                saved.rateHz = o.value(QLatin1String("rate_hz")).toDouble();
                for (const QJsonValue &fv : o.value(QLatin1String("fields")).toArray()) {
                    const QJsonObject fo = fv.toObject();
                    Field field;
                    field.first = fo.value(QLatin1String("first")).toInt() != 0;
                    if (fo.contains(QLatin1String("code"))) {
                        field.code = fo.value(QLatin1String("code")).toString().toStdString();
                    }
                    for (const QJsonValue &lv : fo.value(QLatin1String("lines")).toArray()) {
                        field.lines.push_back(lv.toDouble());
                    }
                    saved.fields.push_back(std::move(field));
                }
                if (!saved.fields.empty()) {
                    m_pass0[static_cast<size_t>(track)] = std::move(saved);
                }
            }
        }
    }    m_blocks.resize(m_meta.sources.size());
    for (const ebml::BlockRef &b : idx.blocks) {
        if (b.track >= 1 && b.track <= m_meta.sources.size()) {
            m_blocks[static_cast<size_t>(b.track) - 1][b.ts] = {b.off, b.len};
        }
    }
    for (size_t k = 0; k < m_meta.sources.size(); ++k) {
        for (const size_t j : m_meta.sources[k].refs) {
            if (m_tables.find({k, j}) == m_tables.end()) {
                throw Error("the lag table of track " + std::to_string(k) + " against track " + std::to_string(j) + " is missing");
            }
        }
    }
}

uint64_t Archive::packedBytes(size_t k) const
{
    uint64_t total = 0;
    for (const auto &b : m_blocks[k]) {
        total += b.second.length;
    }
    return total;
}

std::string archiveInfo(const QString &path)
{
    Archive a(path);
    const ArchiveMeta &m = a.meta();
    std::string out = format("%s: %zu tracks, segments of %zu samples\n", path.toStdString().c_str(), m.sources.size(), m.segment);
    for (size_t k = 0; k < m.sources.size(); ++k) {
        const SourceMeta &sm = m.sources[k];
        const uint64_t packed = a.packedBytes(k);
        out += format("  %zu: %s  %llu bytes -> %llu (%s bits/sample)  shift %u mean %d  refs %s  predictor own %zu taps %zu stride %zu every %zu\n",
                         k, sm.name.c_str(), static_cast<unsigned long long>(sm.size), static_cast<unsigned long long>(packed),
                         bitsPerSample(packed, sm.words).c_str(), sm.shift, sm.mean, listOfNumbers(sm.refs).c_str(), sm.own,
                         sm.taps, sm.stride, sm.every);
        out += "     sha256 " + sm.sha256 + "\n";
        std::string noise;
        if (sm.noise) {
            noise = format("  noise %.3f", *sm.noise);
        }
        std::string side = "[";
        for (size_t i = 0; i < sm.sidecars.size(); ++i) {
            side += (i ? ", " : "") + jsonQuote(sm.sidecars[i].name);
        }
        side += "]";
        out += format("     on the command line: %zu%s  sidecars %s\n", sm.position, noise.c_str(), side.c_str());
        if (sm.hasInfo()) {
            out += "     disc: " + describeInfo(sm) + "\n";
        }
        const auto lines = a.pass0().find(k);
        if (lines != a.pass0().end()) {
            out += format("     TBC curve kept: %s, %zu fields (pass 0)\n", lines->second.pal ? "PAL" : "NTSC", lines->second.fields.size());
        }
    }
    return out;
}

std::string archiveYaml(const QString &path)
{
    Archive a(path);
    const ArchiveMeta &m = a.meta();
    Node tracks = Node::array();
    for (size_t k = 0; k < m.sources.size(); ++k) {
        const SourceMeta &sm = m.sources[k];
        const uint64_t packed = a.packedBytes(k);
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
        t.set("own", Node::unsignedInteger(sm.own));
        t.set("solo", Node::unsignedInteger(sm.solo));
        t.set("taps", Node::unsignedInteger(sm.taps));
        t.set("ntop", Node::unsignedInteger(sm.ntop));
        t.set("ttaps", Node::unsignedInteger(sm.ttaps));
        t.set("stride", Node::unsignedInteger(sm.stride));
        t.set("every", Node::unsignedInteger(sm.every));
        t.set("lam_bits", Node::unsignedInteger(sm.lamBits));
        Node refs = Node::array();
        for (const size_t r : sm.refs) {
            refs.push(Node::unsignedInteger(r));
        }
        t.set("refs", refs);
        Node weights = Node::array();
        for (const float w : sm.weights) {
            weights.push(Node::real(static_cast<double>(w)));
        }
        t.set("weights", weights);
        t.set("position", Node::unsignedInteger(sm.position));
        t.set("noise", sm.noise ? Node::real(*sm.noise) : Node());
        t.set("origin", sm.origin ? Node::string(*sm.origin) : Node());
        t.set("rate_hz", Node::real(sm.rateHz));
        if (!sm.notes.empty()) {
            t.set("notes", Node::string(sm.notes));
        }
        if (sm.bits > 0) {
            t.set("bits", Node::unsignedInteger(sm.bits));
        }
        if (sm.fileTime) {
            t.set("file_time", Node::string(*sm.fileTime));
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
        t.set("track", Node::unsignedInteger(k));
        t.set("packed_bytes", Node::unsignedInteger(packed));
        t.set("bits_per_sample", Node::real(static_cast<double>(packed) * 8.0 / static_cast<double>(std::max<uint64_t>(sm.words, 1))));
        Node sidecars = Node::array();
        for (const Sidecar &sc : sm.sidecars) {
            Node e = Node::object();
            e.set("name", Node::string(sc.name));
            e.set("sha256", Node::string(sc.sha256));
            e.set("bytes", Node::unsignedInteger(sc.data.size()));
            const QByteArray raw(reinterpret_cast<const char *>(sc.data.data()), static_cast<qsizetype>(sc.data.size()));
            // Text only if the bytes are valid UTF-8.
            const QString text = QString::fromUtf8(raw);
            if (text.toUtf8() == raw) {
                e.set("text", Node::string(text.toStdString()));
            }
            sidecars.push(e);
        }
        t.set("sidecars", sidecars);
        Node tables = Node::array();
        for (const size_t j : sm.refs) {
            const auto it = a.tables().find({k, j});
            if (it != a.tables().end()) {
                Node e = Node::object();
                e.set("reference", Node::unsignedInteger(j));
                e.set("steps", Node::unsignedInteger(it->second.pos.size()));
                tables.push(e);
            }
        }
        t.set("lag_tables", tables);
        tracks.push(t);
    }
    Node doc = Node::object();
    doc.set("format", Node::string(m.format));
    doc.set("version", Node::unsignedInteger(m.version));
    doc.set("segment", Node::unsignedInteger(m.segment));
    doc.set("tracks", tracks);
    std::string out;
    doc.writeYaml(0, out);
    return out;
}

void unpackArchive(const QString &path, const QString &outdir, const UnpackOptions &options)
{
    Archive a(path);
    const ArchiveMeta &meta = a.meta();
    if (meta.sources.empty()) {
        throw Error("the archive holds no track");
    }
    if (!QDir().mkpath(outdir)) {
        throw Error("cannot create " + outdir.toStdString());
    }
    const size_t last = options.only.value_or(meta.sources.size() - 1);
    if (last >= meta.sources.size()) {
        throw Error("the archive has " + std::to_string(meta.sources.size()) + " tracks");
    }
    const QDir dir(outdir);
    std::vector<std::unique_ptr<PosFile>> outs;
    std::vector<QString> names;
    for (size_t k = 0; k <= last; ++k) {
        const SourceMeta &sm = meta.sources[k];
        names.push_back(dir.filePath(safeName(sm.name, "track")));
        outs.push_back(std::make_unique<PosFile>(names.back(), PosFile::Mode::Write));
        outs.back()->setSize(sm.size);
    }
    std::vector<PosFile *> files;
    for (auto &f : outs) {
        files.push_back(f.get());
    }
    const ReferenceWindows windows(files, meta, a.tables());
    const size_t s = meta.segment;
    const unsigned threads = options.threads != 0 ? options.threads : defaultThreads();
    for (size_t k = 0; k <= last; ++k) {
        const SourceMeta &sm = meta.sources[k];
        const Preset pre = sm.preset();
        const bool useRef = !sm.refs.empty();
        const size_t nseg = static_cast<size_t>((sm.words + s - 1) / s);
        const auto t0 = std::chrono::steady_clock::now();
        const auto &blocks = a.blocks(k);
        if (options.device == Device::Gpu) {
            gpuUnpackTrack(k, a, windows, *outs[k], threads);
        }
        parallelFor(options.device == Device::Gpu ? 0 : nseg, threads, [&](size_t g) {
            const size_t s0 = g * s;
            const size_t n = std::min<size_t>(s, static_cast<size_t>(sm.words) - s0);
            const auto block = blocks.find(g);
            if (block == blocks.end()) {
                throw Error(sm.name + ": segment " + std::to_string(g) + " is missing");
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(block->second.length));
            if (!bytes.empty()) {
                a.file().readAt(bytes.data(), bytes.size(), block->second.offset);
            }
            std::vector<float> window;
            if (useRef) {
                window = windows.window(k, static_cast<int64_t>(s0), n);
            }
            std::vector<int32_t> residuals;
            if (!decodeResiduals(bytes.data(), bytes.size(), n, residuals)) {
                throw Error(sm.name + ": segment " + std::to_string(g) + " is corrupt");
            }
            std::vector<int32_t> y = reconstruct(residuals, pre, useRef, window.data(), n + 2 * sm.taps);
            for (int32_t &v : y) {
                v = static_cast<int32_t>((static_cast<uint32_t>(v) + static_cast<uint32_t>(sm.mean)) << sm.shift);
            }
            storeSamples(sm.format(), *outs[k], s0, y.data(), y.size());
        });
        if (!sm.tail.empty()) {
            outs[k]->writeAt(sm.tail.data(), sm.tail.size(), formatCovered(sm.format(), sm.size));
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const std::string got = hashBytes(*outs[k], sm.size);
        if (got != sm.sha256) {
            throw Error(sm.name + ": SHA-256 differs from the original (" + got + " against " + sm.sha256 + ")");
        }
        std::fprintf(stderr, "%s: %llu bytes, SHA-256 matches, %.1f Msmp/s\n", sm.name.c_str(), static_cast<unsigned long long>(sm.size),
                     static_cast<double>(sm.words) / std::max(secs, 1e-9) / 1e6);
        if (!options.only || *options.only == k) {
            for (const Sidecar &sc : sm.sidecars) {
                if (hexSha256(sc.data) != sc.sha256) {
                    throw Error(sm.name + ": the stored copy of " + sc.name + " is damaged");
                }
                QFile out(dir.filePath(safeName(sc.name, "sidecar")));
                if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    || out.write(reinterpret_cast<const char *>(sc.data.data()), static_cast<qint64>(sc.data.size())) != static_cast<qint64>(sc.data.size())) {
                    throw Error("cannot write " + out.fileName().toStdString());
                }
            }
        }
    }
    outs.clear();
    if (options.only) {
        // The earlier tracks were decoded as references only.
        for (size_t k = 0; k <= last; ++k) {
            if (k != *options.only) {
                QFile::remove(names[k]);
            }
        }
    }
    // Another format asked for: the files above are the captures as they were
    // given; each is converted sample by sample and checked by the hash of its
    // samples.
    if (options.target && options.target->kind != OutputFormat::Kind::MultichannelFlac) {
        for (size_t k = 0; k <= last; ++k) {
            const SourceMeta &sm = meta.sources[k];
            if ((options.only && *options.only != k)
                || (options.target->kind == OutputFormat::Kind::Raw && options.target->raw == sm.format())) {
                continue;
            }
            if (options.target->kind == OutputFormat::Kind::Raw) {
                convertFile(names[k], sm, options.target->raw, outdir);
            } else {
                encodeFlacFile(names[k], sm, options.target->kind == OutputFormat::Kind::Ldf, outdir);
            }
        }
    }
}

void unpackMetadata(const QString &path, const QString &outdir, std::optional<size_t> only)
{
    Archive a(path);
    if (!QDir().mkpath(outdir)) {
        throw Error("cannot create " + outdir.toStdString());
    }
    const QDir dir(outdir);
    for (size_t k = 0; k < a.meta().sources.size(); ++k) {
        if (only && *only != k) {
            continue;
        }
        const SourceMeta &sm = a.meta().sources[k];
        for (const Sidecar &sc : sm.sidecars) {
            if (hexSha256(sc.data) != sc.sha256) {
                throw Error(sm.name + ": the stored " + sc.name + " does not match its hash");
            }
            const QString target = dir.filePath(safeName(sc.name, "sidecar"));
            QFile out(target);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
                || out.write(reinterpret_cast<const char *>(sc.data.data()), static_cast<qint64>(sc.data.size())) != static_cast<qint64>(sc.data.size())) {
                throw Error("cannot write " + target.toStdString());
            }
            std::printf("%s\n", QDir::toNativeSeparators(target).toStdString().c_str());
        }
    }
    const QString stem = QFileInfo(path).fileName();
    const QString target = dir.filePath(stem + QStringLiteral(".yaml"));
    const std::string yaml = archiveYaml(path);
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || out.write(yaml.data(), static_cast<qint64>(yaml.size())) != static_cast<qint64>(yaml.size())) {
        throw Error("cannot write " + target.toStdString());
    }
    std::printf("%s\n", QDir::toNativeSeparators(target).toStdString().c_str());
}

void savePass0(const QString &path, const std::map<size_t, SavedPass0> &tracks)
{
    // (the archive is read first: it must be one, and the metadata track is the one after the data tracks)
    uint64_t metaTrack = 0;
    {
        const Archive archive(path);
        metaTrack = archive.meta().sources.size() + 1;
    }
    QJsonArray list;
    for (const auto &entry : tracks) {
        QJsonArray fields;
        for (const Field &f : entry.second.fields) {
            QJsonArray lines;
            for (const double x : f.lines) {
                lines.append(x);   // (as it is, to the bit: a decoder that takes the lines from the file gets what it would have located)
            }
            QJsonObject fo{{QLatin1String("first"), f.first ? 1 : 0}, {QLatin1String("lines"), lines}};
            if (f.code) {
                fo.insert(QLatin1String("code"), QString::fromStdString(*f.code));
            }
            fields.append(fo);
        }
        list.append(QJsonObject{{QLatin1String("track"), static_cast<int>(entry.first)},
                                {QLatin1String("system"), QLatin1String(entry.second.pal ? "pal" : "ntsc")},
                                {QLatin1String("rate_hz"), entry.second.rateHz},
                                {QLatin1String("fields"), fields}});
    }
    const QByteArray json = QJsonDocument(QJsonObject{{QLatin1String("pass0"), list}}).toJson(QJsonDocument::Compact);    const ebml::Bytes block = ebml::cluster(kSpeedTimestamp, metaTrack, ebml::Bytes(json.begin(), json.end()));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        throw Error(path.toStdString() + ": cannot be written: " + file.errorString().toStdString());
    }
    if (file.write(reinterpret_cast<const char *>(block.data()), static_cast<qint64>(block.size())) != static_cast<qint64>(block.size()) || !file.flush()) {
        throw Error(path.toStdString() + ": the TBC curve could not be written: " + file.errorString().toStdString());
    }
}

} // namespace rfx
