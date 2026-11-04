/******************************************************************************
 * requantizejob.cpp
 * rfx library - whole requantising jobs: a capture in, the same capture
 * requantised out; an .rfx file in, an .rfx file out
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools. See requantize.h.
 ******************************************************************************/

#include "archive.h"
#include "capture.h"
#include "error.h"
#include "ffmpeg.h"
#include "flacmode.h"
#include "packer.h"
#include "parallel.h"
#include "requantize.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace rfx {

namespace {

constexpr double kMegahertz = 1.0e6;
constexpr double kHertzPerKilohertz = 1000.0;
constexpr double kBehindRealTimeSeconds = 0.5;
constexpr const char *kStandardStream = "-";
constexpr const char *kStandardTrackName = "stdin.s16";

enum class OutKind { None, Standard, Archive, Raw, Flac, Ldf };

struct Output
{
    OutKind kind = OutKind::None;
    SampleFormat raw = SampleFormat::S16;
};

// A track as the job sees it: the samples to read, and what is known of them.
struct Track
{
    std::string name;
    // The file to read; empty for standard input.
    QString path;
    SampleFormat fmt = SampleFormat::S16;
    uint64_t words = 0;
    double rateHz = 0.0;
    std::optional<int> bits;
    std::vector<Sidecar> sidecars;
    // Its place among the tracks of the input.
    size_t place = 0;
};

void say(std::FILE *report, const std::string &text)
{
    if (report) {
        std::fputs(text.c_str(), report);
        std::fflush(report);
    }
}

bool isCompressedName(const std::string &name)
{
    return name == "flac" || name == "ldf";
}

// A .flac or .ldf capture, which ffmpeg decodes.
bool isCompressedInput(const RequantizeJob &job)
{
    return isCompressedName(job.format) || (job.format == "auto" && isCompressedName(QFileInfo(job.input).suffix().toLower().toStdString()));
}

Output classifyOutput(const QString &path)
{
    Output out;
    if (path.isEmpty()) {
        return out;
    }
    if (path == QLatin1String(kStandardStream)) {
        out.kind = OutKind::Standard;
        return out;
    }
    const std::string suffix = QFileInfo(path).suffix().toLower().toStdString();
    if (suffix == "rfx") {
        out.kind = OutKind::Archive;
    } else if (suffix == "flac") {
        out.kind = OutKind::Flac;
    } else if (suffix == "ldf") {
        out.kind = OutKind::Ldf;
    } else if (parseFormat(suffix, out.raw)) {
        out.kind = OutKind::Raw;
    } else {
        throw UsageError(path.toStdString() + " is not somewhere a requantised capture can be written: give a file of the format of the input (.rfx .flac .ldf .s16 .r16 .s8 .r8 .lds), or - for standard output.");
    }
    return out;
}

// The container rate is the RF rate in kHz (75 MHz -> 75000 Hz), as ld-decode reads it.
QString containerRate(double rateHz)
{
    return QString::number(static_cast<qulonglong>(std::llround(rateHz / kHertzPerKilohertz)));
}

// The requantised raw samples back into the codec they came from.
void encodeFlac(const QString &raw, const QString &destination, bool ogg, double rateHz, int compression,
                const std::vector<std::pair<std::string, std::string>> &tags)
{
    QStringList args = {"-v", "error", "-y", "-f", "s16le", "-ar", containerRate(rateHz), "-ac", "1", "-i", raw, "-c:a", "flac", "-compression_level", QString::number(compression)};
    for (const auto &tag : tags) {
        args << "-metadata" << QString::fromStdString(tag.first + "=" + tag.second);
    }
    if (ogg) {
        args << "-f" << "ogg";
    }
    args << destination;
    runFfmpeg(args, "ffmpeg could not encode " + destination.toStdString());
}

QString trackFile(const QString &dir, const std::string &name)
{
    const QString file = QFileInfo(QString::fromStdString(name)).fileName();
    if (file.isEmpty() || file == QLatin1String(".") || file == QLatin1String("..")) {
        throw Error("bad track name: " + name);
    }
    return QDir(dir).filePath(file);
}

std::string percent(uint64_t part, uint64_t whole)
{
    if (whole == 0) {
        return "0%";
    }
    return formatDecimal(100.0 * static_cast<double>(part) / static_cast<double>(whole), 1) + "%";
}

std::string byteCount(uint64_t bytes)
{
    return std::to_string(bytes) + " bytes";
}

// The tracks of an .rfx input, decoded to `work` with their sidecars.
std::vector<Track> tracksOfArchive(const RequantizeJob &job, const QString &work)
{
    const bool flac = isFlacArchive(job.input);
    const std::vector<SourceMeta> sources = flac ? readFlacMeta(job.input) : Archive(job.input).meta().sources;
    if (sources.empty()) {
        throw Error(job.input.toStdString() + " holds no track");
    }
    std::vector<size_t> chosen = job.only;
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    for (const size_t k : chosen) {
        if (k >= sources.size()) {
            throw UsageError(job.input.toStdString() + " has " + std::to_string(sources.size()) + " tracks: no track " + std::to_string(k));
        }
    }
    if (chosen.empty()) {
        for (size_t k = 0; k < sources.size(); ++k) {
            chosen.push_back(k);
        }
    }

    std::optional<size_t> single;
    if (chosen.size() == 1) {
        single = chosen.front();
    }
    if (flac) {
        unpackFlac(job.input, work, single, std::nullopt);
    } else {
        UnpackOptions unpack;
        unpack.only = single;
        unpack.device = job.pack.device;
        unpack.threads = job.pack.threads;
        unpackArchive(job.input, work, unpack);
    }

    std::vector<Track> tracks;
    for (const size_t k : chosen) {
        const SourceMeta &sm = sources[k];
        Track t;
        t.name = QFileInfo(QString::fromStdString(sm.name)).fileName().toStdString();
        t.path = trackFile(work, sm.name);
        t.fmt = sm.format();
        t.words = sm.words;
        t.rateHz = sm.rateHz;
        t.bits = bitsOfFormat(t.fmt, sm.shift);
        t.sidecars = sm.sidecars;
        t.place = k;
        if (!QFileInfo(t.path).isFile()) {
            throw Error("track " + std::to_string(k) + " was not decoded");
        }
        tracks.push_back(std::move(t));
    }
    return tracks;
}

Track trackOfCapture(const RequantizeJob &job, const QString &work, std::FILE *report)
{
    Track t;
    t.sidecars = findSidecars(job.input);
    if (const std::optional<double> hz = sidecarRateHz(t.sidecars)) {
        t.rateHz = *hz;
    }
    if (isCompressedInput(job)) {
        // The samples are read at any position, and ffmpeg gives them in order:
        // through a raw copy.
        t.fmt = SampleFormat::S16;
        t.name = (QFileInfo(job.input).completeBaseName() + QStringLiteral(".s16")).toStdString();
        t.path = QDir(work).filePath(QString::fromStdString(t.name));
        if (!QFileInfo(job.input).isFile()) {
            throw Error("Could not open " + job.input.toStdString());
        }
        say(report, "decoding " + job.input.toStdString() + " to raw 16-bit samples\n");
        runFfmpeg({"-v", "error", "-y", "-i", job.input, "-f", "s16le", "-acodec", "pcm_s16le", t.path}, "ffmpeg could not decode " + job.input.toStdString());
    } else {
        t.fmt = inputFormat(job.input, job.format);
        t.name = QFileInfo(job.input).fileName().toStdString();
        t.path = job.input;
    }
    const QFileInfo info(t.path);
    if (!info.isFile()) {
        throw Error("Could not open " + job.input.toStdString());
    }
    t.words = formatSamples(t.fmt, static_cast<uint64_t>(info.size()));
    return t;
}

} // namespace

std::vector<RequantizeResult> runRequantize(const RequantizeJob &job)
{
    std::FILE *const report = job.report;
    const bool fromStandard = job.input == QLatin1String(kStandardStream);
    const bool isArchive = !fromStandard && QFileInfo(job.input).suffix().compare(QLatin1String("rfx"), Qt::CaseInsensitive) == 0;
    const Output output = classifyOutput(job.output);

    if (job.input.isEmpty()) {
        throw UsageError("No capture was given to requantise.");
    }
    // The tool does not change formats: what comes out is coded as what went in
    // (a compressed capture is decoded to be read, and encoded again with the same
    // codec).
    const bool compressed = !fromStandard && !isArchive && isCompressedInput(job);
    if (isArchive && output.kind != OutKind::None && output.kind != OutKind::Archive) {
        throw UsageError("An .rfx file is requantised to an .rfx file: the tool does not change formats. Unpack the result with rfx unpack to get the captures.");
    }
    if (compressed) {
        const std::string codec = job.format == "auto" ? QFileInfo(job.input).suffix().toLower().toStdString() : job.format;
        const OutKind same = codec == "ldf" ? OutKind::Ldf : OutKind::Flac;
        if (output.kind != OutKind::None && output.kind != same) {
            throw UsageError(std::string("A .") + codec + " capture is requantised to a ." + codec + " file: the tool does not change formats.");
        }
    } else if (!isArchive && output.kind != OutKind::None && output.kind != OutKind::Standard && output.kind != OutKind::Raw) {
        throw UsageError("A raw capture is requantised to a raw capture of the same format: the tool does not change formats. Pack the result with rfx pack, or encode it.");
    }
    if (!job.output.isEmpty() && job.output != QLatin1String(kStandardStream) && !fromStandard) {
        const QFileInfo in(job.input);
        const QFileInfo out(job.output);
        if (in.absoluteFilePath() == out.absoluteFilePath() || (out.exists() && in.canonicalFilePath() == out.canonicalFilePath())) {
            throw UsageError("The output would overwrite the capture it is made from.");
        }
    }
    if (fromStandard && !job.rateMhz) {
        throw UsageError("The samples' rate is not known: give it with --rate, in MHz.");
    }
    if (fromStandard && !job.standardInput) {
        throw Error("no standard input");
    }
    if (output.kind == OutKind::Standard && !job.standardOutput) {
        throw Error("no standard output");
    }
    if (!job.only.empty() && !isArchive) {
        throw UsageError("--only picks tracks of an .rfx file");
    }

    // An .rfx file and a compressed capture are decoded to files: beside the
    // output, so that a big capture lands on the disk it is going to, or in the
    // system's temporary directory when nothing is written.
    std::unique_ptr<QTemporaryDir> work;
    QString inputDir;
    QString outputDir;
    if (isArchive || compressed) {
        const bool written = output.kind != OutKind::None && output.kind != OutKind::Standard;
        const QString base = written ? QFileInfo(job.output).absoluteDir().absolutePath() : QDir::tempPath();
        work = std::make_unique<QTemporaryDir>(QDir(base).filePath(QStringLiteral("rf-requantize-XXXXXX")));
        if (!work->isValid()) {
            throw Error("cannot create a work directory in " + base.toStdString());
        }
        inputDir = QDir(work->path()).filePath(QStringLiteral("in"));
        outputDir = QDir(work->path()).filePath(QStringLiteral("out"));
        if (!QDir().mkpath(inputDir) || !QDir().mkpath(outputDir)) {
            throw Error("cannot create a work directory in " + work->path().toStdString());
        }
    }

    // The tracks, and what is known of each.
    std::vector<Track> tracks;
    if (fromStandard) {
        Track t;
        t.name = kStandardTrackName;
        tracks.push_back(std::move(t));
    } else if (isArchive) {
        tracks = tracksOfArchive(job, inputDir);
    } else {
        tracks.push_back(trackOfCapture(job, inputDir, report));
    }
    for (Track &t : tracks) {
        if (job.rateMhz) {
            t.rateHz = *job.rateMhz * kMegahertz;
        }
        if (!(t.rateHz > 0.0)) {
            throw UsageError("The rate of " + t.name + " is not known: give it with --rate, in MHz.");
        }
    }
    if (output.kind == OutKind::Raw && output.raw != tracks.front().fmt) {
        throw UsageError(job.output.toStdString() + " is not in the format of the input (" + formatName(tracks.front().fmt)
                         + "): the tool does not change formats.");
    }
    if (output.kind == OutKind::Standard && tracks.front().fmt != SampleFormat::S16) {
        throw UsageError(std::string("Standard output carries signed 16-bit samples, and the input is ") + formatName(tracks.front().fmt)
                         + ": the tool does not change formats.");
    }

    // The CSV log is written from one track at a time.
    std::unique_ptr<QFile> logFile;
    if (!job.logPath.isEmpty()) {
        logFile = std::make_unique<QFile>(job.logPath);
        if (!logFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            throw Error("Could not create " + job.logPath.toStdString());
        }
        logFile->write(requantizeLogHeader());
    }

    std::vector<RequantizeResult> results(tracks.size());
    std::vector<QString> packed(tracks.size());
    unsigned threads = job.pack.threads != 0 ? job.pack.threads : defaultThreads();
    if (logFile) {
        threads = 1;
    }

    // The sidecars that sat beside the captures go beside what is written, where
    // pack finds them.
    if (output.kind == OutKind::Archive) {
        for (const Track &t : tracks) {
            for (const Sidecar &sc : t.sidecars) {
                QFile file(QDir(outputDir).filePath(QFileInfo(QString::fromStdString(sc.name)).fileName()));
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    || file.write(reinterpret_cast<const char *>(sc.data.data()), static_cast<qint64>(sc.data.size())) != static_cast<qint64>(sc.data.size())) {
                    throw Error("cannot write " + file.fileName().toStdString());
                }
            }
        }
    }

    const bool single = tracks.size() == 1;
    parallelFor(tracks.size(), threads, [&](size_t i) {
        const Track &t = tracks[i];
        StreamSpec spec;
        spec.name = t.name;
        spec.rateMhz = t.rateHz / kMegahertz;
        spec.knownBits = t.bits;
        spec.total = t.words;
        spec.logTrack = static_cast<int>(t.place);
        if (logFile) {
            spec.log = [&logFile](const std::string &line) { logFile->write(line.data(), static_cast<qint64>(line.size())); };
        }
        if (single && job.showProgress && report) {
            spec.progress = [report](int p) {
                std::fprintf(report, "  %d%%\r", p);
                std::fflush(report);
            };
        }

        std::unique_ptr<PosFile> in;
        SegmentReader reader;
        if (fromStandard) {
            reader = standardReader(job.standardInput);
        } else {
            in = std::make_unique<PosFile>(t.path, PosFile::Mode::Read);
            reader = fileReader(*in, t.fmt, t.words, t.name);
        }

        // Where the requantised samples go: in the format they came in.
        std::unique_ptr<PosFile> out;
        SegmentWriter writer;
        switch (output.kind) {
        case OutKind::None: break;
        case OutKind::Standard: writer = standardWriter(job.standardOutput); break;
        case OutKind::Raw: out = std::make_unique<PosFile>(job.output, PosFile::Mode::Write); break;
        case OutKind::Archive:
        case OutKind::Flac:
        case OutKind::Ldf:
            // Raw files to code again (the compressed capture is 16-bit raw).
            packed[i] = trackFile(outputDir, t.name);
            out = std::make_unique<PosFile>(packed[i], PosFile::Mode::Write);
            break;
        }
        if (out) {
            writer = fileWriter(*out, t.fmt, t.name);
        }

        results[i] = requantizeStream(job.options, spec, reader, writer);
        if (out && in) {
            copyTail(*in, *out, t.fmt);
        }
    });
    if (logFile) {
        logFile->close();
    }

    // What was asked, as the capture application records it.
    RequantizerRequest request;
    request.sampleRateMhz = tracks.front().rateHz / kMegahertz;
    request.marginLevel = job.options.marginLevel;
    request.bands = job.options.bands;
    request.adaptive = job.options.adaptive;
    request.depthsDb = job.options.depthsDb;
    request.order = job.options.order;
    const auto tags = requantizationTags(job.options, settingsFor(request));

    // The requantised tracks are coded again as the input was. An .rfx file of the
    // predictor variant keeps the lines where they were (the lag tables are the
    // archive's, so ld-decode is not needed); one of the FLAC variant has no
    // prediction between tracks, and a compressed capture is encoded with the
    // codec it came in.
    std::vector<PackedTrack> packedTracks;
    uint64_t rawBytes = 0;
    for (const QString &p : packed) {
        rawBytes += p.isEmpty() ? 0 : static_cast<uint64_t>(QFileInfo(p).size());
    }
    if (output.kind == OutKind::Archive) {
        std::vector<QString> inputs(packed.begin(), packed.end());
        if (isFlacArchive(job.input)) {
            PackOptions options = job.pack;
            options.order = OrderMode::Given;
            options.rateMhz = tracks.front().rateHz / kMegahertz;
            const bool sameFormat = std::all_of(tracks.begin(), tracks.end(), [&](const Track &t) { return t.fmt == tracks.front().fmt; });
            options.format = sameFormat ? formatName(tracks.front().fmt) : "auto";
            for (const Track &t : tracks) {
                if (std::fabs(t.rateHz - tracks.front().rateHz) > 1.0) {
                    say(report, "Warning: the tracks were not captured at one rate; the rate of " + tracks.front().name + " is used where a track has no .yaml.\n");
                    break;
                }
            }
            packFlac(inputs, job.output, options);
        } else {
            std::vector<size_t> places;
            for (const Track &t : tracks) {
                places.push_back(t.place);
            }
            std::optional<double> rateHz;
            if (job.rateMhz) {
                rateHz = *job.rateMhz * kMegahertz;
            }
            packedTracks = packCapturesLike(inputs, job.input, places, job.output, job.pack, rateHz);
        }
    } else if (output.kind == OutKind::Flac || output.kind == OutKind::Ldf) {
        encodeFlac(packed.front(), job.output, output.kind == OutKind::Ldf, tracks.front().rateHz, job.flacCompression, tags);
    }

    // What it came to.
    for (size_t i = 0; i < tracks.size(); ++i) {
        const RequantizeResult &r = results[i];
        const RequantizationStatus::Summary &s = r.summary;
        std::string text = std::string(job.options.requantize ? "Requantised " : "Copied, as is, ") + r.name + " at "
            + formatDecimal(r.rateMhz, 3) + " Msps, " + std::to_string(r.inputBits) + " bits in\n";
        if (i == 0) {
            for (const auto &tag : tags) {
                text += "  " + tag.first + ": " + tag.second + "\n";
            }
        }
        text += "Read " + std::to_string(s.samples) + " samples (" + formatDecimal(r.lastsSeconds, 1) + " s) in " + std::to_string(s.segments)
            + " segments\n";
        if (job.options.requantize) {
            text += "  Bits dropped:";
            for (size_t drop = s.samplesByDrop.size(); drop-- > 0;) {
                if (s.samplesByDrop[drop] > 0) {
                    text += " " + std::to_string(drop) + " for " + percent(s.samplesByDrop[drop], s.samples);
                }
            }
            text += "\n  Shaped: " + percent(s.shapedSamples, s.samples) + "; worst rise in the protected bands "
                + formatDecimal(s.worstDegradationDb, 2) + " dB; " + std::to_string(s.clipped) + " samples clipped\n";
        } else {
            text += "  Not requantised: the reference to compare the others with.\n";
        }
        // Paced, it should have taken as long as the samples last. Much longer is a
        // requantiser, or a reader downstream, that could not keep up, which is what
        // a live capture with these settings would have run into too.
        if (job.options.realtime && r.tookSeconds > r.lastsSeconds + kBehindRealTimeSeconds) {
            text += "Behind real time: " + formatDecimal(r.tookSeconds, 1) + " s for " + formatDecimal(r.lastsSeconds, 1) + " s of samples\n";
        }
        say(report, text);
    }
    switch (output.kind) {
    case OutKind::None: say(report, "Nothing written: give an output to keep the result.\n"); break;
    case OutKind::Standard: break;
    case OutKind::Archive: {
        uint64_t before = 0;
        uint64_t after = 0;
        for (const PackedTrack &p : packedTracks) {
            before += p.size;
            after += p.packed;
        }
        if (packedTracks.empty()) {
            // The FLAC variant: the file is what it took.
            before = rawBytes;
            after = static_cast<uint64_t>(QFileInfo(job.output).size());
        }
        say(report, "Wrote " + job.output.toStdString() + ": " + byteCount(after) + " packed from " + byteCount(before) + " ("
                + formatDecimal(100.0 * static_cast<double>(after) / static_cast<double>(std::max<uint64_t>(before, 1)), 1) + "%)\n");
        break;
    }
    case OutKind::Flac:
    case OutKind::Ldf: {
        const uint64_t bytes = static_cast<uint64_t>(QFileInfo(job.output).size());
        say(report, "Wrote " + job.output.toStdString() + ": " + byteCount(bytes) + ", "
                + formatDecimal(8.0 * static_cast<double>(bytes) / static_cast<double>(std::max<uint64_t>(results.front().summary.samples, 1)), 2)
                + " bits a sample\n");
        break;
    }
    case OutKind::Raw:
        say(report, "Wrote " + job.output.toStdString() + ": " + byteCount(static_cast<uint64_t>(QFileInfo(job.output).size()))
                + ", uncompressed: pack it with rfx pack to see what was saved\n");
        break;
    }
    return results;
}

} // namespace rfx
