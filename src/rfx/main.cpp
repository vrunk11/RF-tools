/******************************************************************************
 * main.cpp
 * rfx - several RF captures of one disc in one lossless .rfx file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "archive.h"
#include "diagnostics.h"
#include "edit.h"
#include "error.h"
#include "flacmode.h"
#include "capture.h"
#include "packer.h"
#include "rfsync.h"
#include "trackreader.h"
#include "verify.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <optional>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// Exit codes (AGENTS.md 4.4): 0 done, 1 the work failed, 2 the command line is wrong.
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

bool parseDevice(const QString &name, rfx::Device &out)
{
    if (name == QLatin1String("cpu")) {
        out = rfx::Device::Cpu;
    } else if (name == QLatin1String("gpu")) {
        out = rfx::Device::Gpu;
    } else {
        return false;
    }
    return true;
}

int usageError(const QString &message)
{
    std::fprintf(stderr, "rfx: %s\n", message.toStdString().c_str());
    return kExitUsage;
}

// The value of an option that is a track number; false if it is not one.
bool trackOption(const QCommandLineParser &parser, const QCommandLineOption &option, std::optional<size_t> &out, QString &error)
{
    if (!parser.isSet(option)) {
        return true;
    }
    bool ok = false;
    const uint value = parser.value(option).toUInt(&ok);
    if (!ok) {
        error = QStringLiteral("--only wants a track number, not '%1'").arg(parser.value(option));
        return false;
    }
    out = value;
    return true;
}

int runInfo(const QStringList &args)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Show what an .rfx file holds, without decoding a sample.");
    parser.addHelpOption();
    const QCommandLineOption yaml("yaml", "Print the whole metadata as YAML instead.");
    parser.addOption(yaml);
    parser.addPositionalArgument("archive", "The .rfx file.");
    parser.process(args);
    if (parser.positionalArguments().size() != 1) {
        return usageError("info wants one .rfx file");
    }
    const QString path = parser.positionalArguments().first();
    const bool flac = rfx::isFlacArchive(path);
    std::string text;
    if (flac) {
        text = parser.isSet(yaml) ? rfx::flacYaml(path) : rfx::flacInfo(path);
    } else {
        text = parser.isSet(yaml) ? rfx::archiveYaml(path) : rfx::archiveInfo(path);
    }
    std::fputs(text.c_str(), stdout);
    return kExitOk;
}

// What was captured, of the whole file (--source), and whether to treat a capture whose source is not said as a LaserDisc's (--ld).
const QCommandLineOption &sourceOption()
{
    static const QCommandLineOption option("source", "What was captured, of the whole file: ld (a LaserDisc), vhs, vhd, cvbs, or unknown. The TBC curve is worked out for LaserDisc captures only.", "medium");
    return option;
}

const QCommandLineOption &assumeLaserdiscOption()
{
    static const QCommandLineOption option("ld", "Treat the capture as a LaserDisc capture when its source is not said (to work out the TBC curve).");
    return option;
}

// An empty string, or the message to give for a value that is not valid.
QString readMedium(const QCommandLineParser &parser, rfx::PackOptions &out)
{
    out.assumeLaserdisc = parser.isSet(assumeLaserdiscOption());
    if (parser.isSet(sourceOption())) {
        std::string medium;
        if (!rfx::parseMedium(parser.value(sourceOption()).toStdString(), medium)) {
            return QStringLiteral("--source is ld, vhs, vhd, cvbs or unknown, not '%1'").arg(parser.value(sourceOption()));
        }
        out.medium = medium;
    }
    return QString();
}

int runKeepPass0(const QStringList &args)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Work out the TBC curve of the tracks of an .rfx file from their RF (locating the lines, as the pass 0 of ld-decode does) and keep it in the file, as a block added at its end (nothing else is rewritten): the speed of the disc and the time base correction are then there for the programs that read it, without working it out again.");
    parser.addHelpOption();
    const QCommandLineOption all("all", "Work the TBC curve out again for the tracks that have it already (without it they are left as they are).");
    const QCommandLineOption assumeLd("ld", "Treat the capture as a LaserDisc capture when the source of the file is not said (the TBC curve is for LaserDisc captures).");
    const QCommandLineOption rateMhz("rate-mhz", "Sample rate in MHz of a track that has none in the file.", "MHz", "75");
    parser.addOption(all);
    parser.addOption(assumeLd);
    parser.addOption(rateMhz);
    parser.addPositionalArgument("archive", "The .rfx file (a predictor archive).");
    parser.process(args);
    if (parser.positionalArguments().size() != 1) {
        return usageError("tbc-curve wants one .rfx file");
    }
    bool ok = false;
    const double rate = parser.value(rateMhz).toDouble(&ok);
    if (!ok || rate <= 0) {
        return usageError(QStringLiteral("--rate-mhz wants a positive number, not '%1'").arg(parser.value(rateMhz)));
    }
    const size_t done = rfx::keepPass0(parser.positionalArguments().first(), rate, !parser.isSet(all), nullptr, std::nullopt, parser.isSet(assumeLd));
    std::fprintf(stderr, "TBC curve worked out and kept for %zu track(s)\n", done);
    return kExitOk;
}

int runCheck(const QStringList &args)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Check the integrity of .rfx files: every track is decoded and what comes out is compared with the SHA-256 of the samples the file keeps (and each stored sidecar with its own). Nothing is written. The three variants of the file are checked. The exit status is 0 when every track that can be checked is intact, 1 when one is not.");
    parser.addHelpOption();
    const QCommandLineOption track("track", "Check only this track (0 is the primary) of the file; with several files, of each of them.", "track");
    parser.addOption(track);
    parser.addPositionalArgument("archive...", "The .rfx files.");
    parser.process(args);
    const QStringList files = parser.positionalArguments();
    if (files.isEmpty()) {
        return usageError("check wants at least one .rfx file");
    }
    std::optional<size_t> only;
    if (parser.isSet(track)) {
        bool ok = false;
        const uint k = parser.value(track).toUInt(&ok);
        if (!ok) {
            return usageError(QStringLiteral("--track wants a number, not '%1'").arg(parser.value(track)));
        }
        only = k;
    }
    bool allIntact = true;
    for (const QString &file : files) {
        std::atomic<int> shown{-1};
        const std::vector<rfx::TrackCheck> checks = rfx::verifyArchive(file, only, [&shown](double part) {
            // (a line every ten per cent, on the error output, as the other commands tell how far they are)
            const int tenth = static_cast<int>(part * 10.0) * 10;
            if (tenth > shown.exchange(std::max(tenth, shown.load()))) {
                std::fprintf(stderr, "check: %d%%\n", tenth);
            }
        });
        std::fprintf(stdout, "%s\n", QDir::toNativeSeparators(file).toStdString().c_str());
        size_t intact = 0;
        size_t unchecked = 0;
        size_t broken = 0;
        for (const rfx::TrackCheck &c : checks) {
            if (!c.checked) {
                ++unchecked;
                std::fprintf(stdout, "  track %zu %s: no checksum in the file, nothing to compare with\n", c.track, c.name.c_str());
            } else if (c.ok) {
                ++intact;
                std::fprintf(stdout, "  track %zu %s: intact\n", c.track, c.name.c_str());
            } else {
                ++broken;
                std::fprintf(stdout, "  track %zu %s: NOT intact (%s)\n", c.track, c.name.c_str(), c.detail.c_str());
            }
        }
        std::fprintf(stdout, "  %zu intact, %zu not intact, %zu without a checksum\n", intact, broken, unchecked);
        allIntact = allIntact && broken == 0;
    }
    return allIntact ? kExitOk : kExitFailed;
}

int runUnpack(const QStringList &args)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Rebuild the original files of an .rfx file, checking each against its SHA-256.");
    parser.addHelpOption();
    const QCommandLineOption outdir({"d", "outdir"}, "Directory to write into (default: the current one).", "dir", ".");
    const QCommandLineOption only("only", "Extract only this track (0 is the primary).", "track");
    const QCommandLineOption meta("meta", "Write only the metadata (each track's .yaml as it was, and <archive>.yaml with all of it); no sample is decoded.");
    const QCommandLineOption device("device", "Where the predictor runs: cpu or gpu (OpenCL, the same bits).", "device", "cpu");
    const QCommandLineOption threads("threads", "Threads decoding segments (default: one per processor).", "n");
    const QCommandLineOption as("as", "Write the captures in another format (s16, r16, s8, r8, lds, flac, ldf, or mflac: the tracks as the channels of FLAC files, 8 a file, to open in an audio program) instead of the one they came in; the samples are checked by hash.", "format");
    parser.addOption(outdir);
    parser.addOption(only);
    parser.addOption(meta);
    parser.addOption(device);
    parser.addOption(threads);
    parser.addOption(as);
    parser.addPositionalArgument("archive", "The .rfx file.");
    parser.process(args);
    if (parser.positionalArguments().size() != 1) {
        return usageError("unpack wants one .rfx file");
    }
    rfx::Device deviceValue;
    if (!parseDevice(parser.value(device), deviceValue)) {
        return usageError("--device is cpu or gpu");
    }
    rfx::UnpackOptions options;
    options.device = deviceValue;
    QString error;
    if (!trackOption(parser, only, options.only, error)) {
        return usageError(error);
    }
    if (parser.isSet(threads)) {
        bool ok = false;
        const uint n = parser.value(threads).toUInt(&ok);
        if (!ok || n == 0) {
            return usageError(QStringLiteral("--threads wants a positive number, not '%1'").arg(parser.value(threads)));
        }
        options.threads = n;
    }
    const QString path = parser.positionalArguments().first();
    const bool flac = rfx::isFlacArchive(path);
    rfx::OutputFormat format;
    if (parser.isSet(as)) {
        if (!rfx::parseOutputFormat(parser.value(as).toStdString(), format)) {
            return usageError(QStringLiteral("--as: unknown format %1").arg(parser.value(as)));
        }
        options.target = format;
    }
    const QString dir = parser.value(outdir);
    if (parser.isSet(meta)) {
        if (flac) {
            rfx::unpackFlacMetadata(path, dir, options.only);
        } else {
            rfx::unpackMetadata(path, dir, options.only);
        }
    } else if (options.target && options.target->kind == rfx::OutputFormat::Kind::MultichannelFlac) {
        rfx::exportMultichannelFlac(path, dir, options.only);
    } else if (flac) {
        rfx::unpackFlac(path, dir, options.only, options.target);
    } else {
        rfx::unpackArchive(path, dir, options);
    }
    return kExitOk;
}

// The options that say what the disc is and how the signal was captured (what the person knows, not what the samples show): they are told of every
// track the command writes (of a capture that is new, and of a track of a file as well), and only what is given is changed; an empty text, or
// 0 for the copy, takes the value away.
struct DiscOptions
{
    QCommandLineOption standard{"standard", "The video standard of the disc: pal, ntsc, pal-m, secam or ntsc-vhd.", "standard"};
    QCommandLineOption reference{"reference", "A reference of the disc (a catalogue number, a title...).", "text"};
    QCommandLineOption copy{QStringList{"copy", "disc-copy"}, "The number of the physical copy of the disc (the exemplar) the capture was made from (1, 2...).", "n"};
    QCommandLineOption capture{"capture-number", "The number of the capture (a copy of the disc can be captured several times): 1, 2...", "n"};
    QCommandLineOption face{"face", "The face of the disc: A, B, AB or BA.", "face"};
    QCommandLineOption signal{"signal", "The signal that was captured: cvbs (composite) or yc (separate luma and chroma, Y/C).", "signal"};

    void addTo(QCommandLineParser &parser)
    {
        for (const QCommandLineOption *o : {&standard, &reference, &copy, &capture, &face, &signal}) {
            parser.addOption(*o);
        }
    }

    // What was given; an empty string, or the message to give for a value that is not valid.
    QString read(const QCommandLineParser &parser, rfx::InfoEdit &out) const
    {
        std::string value;
        if (parser.isSet(standard)) {
            const std::string text = parser.value(standard).toStdString();
            if (text.empty()) {
                out.standard = std::string();
            } else if (rfx::parseStandard(text, value)) {
                out.standard = value;
            } else {
                return QStringLiteral("--standard is pal, ntsc, pal-m, secam or ntsc-vhd, not '%1'").arg(parser.value(standard));
            }
        }
        if (parser.isSet(reference)) {
            out.reference = parser.value(reference).toStdString();
        }
        if (parser.isSet(copy)) {
            bool ok = false;
            const uint n = parser.value(copy).toUInt(&ok);
            if (!ok) {
                return QStringLiteral("--copy wants a number (0 takes it away), not '%1'").arg(parser.value(copy));
            }
            out.copy = n;
        }
        if (parser.isSet(capture)) {
            bool ok = false;
            const uint n = parser.value(capture).toUInt(&ok);
            if (!ok) {
                return QStringLiteral("--capture-number wants a number (0 takes it away), not '%1'").arg(parser.value(capture));
            }
            out.capture = n;
        }
        if (parser.isSet(face)) {
            const std::string text = parser.value(face).toStdString();
            if (text.empty()) {
                out.face = std::string();
            } else if (rfx::parseFace(text, value)) {
                out.face = value;
            } else {
                return QStringLiteral("--face is A, B, AB or BA, not '%1'").arg(parser.value(face));
            }
        }
        if (parser.isSet(signal)) {
            const std::string text = parser.value(signal).toStdString();
            if (text.empty()) {
                out.signal = std::string();
            } else if (rfx::parseSignal(text, value)) {
                out.signal = value;
            } else {
                return QStringLiteral("--signal is cvbs or yc (Y/C), not '%1'").arg(parser.value(signal));
            }
        }
        return QString();
    }
};

int runPack(const QStringList &args)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Pack captures (the first is the primary) into one .rfx file.");
    parser.addHelpOption();
    const QCommandLineOption output({"o", "output"}, "The .rfx file to write.", "file");
    const QCommandLineOption level({"l", "level"}, "Predictor strength: 1 fastest to 4 strongest.", "n", "3");
    const QCommandLineOption segment("segment", "Samples per independently coded segment.", "n", "262144");
    const QCommandLineOption noVerify("no-verify", "Skip the check that each segment decodes back to itself.");
    const QCommandLineOption pass0("pass0", "Pass 0 dumps already made (pass0_<i>.txt, in the order of the inputs).", "dir");
    const QCommandLineOption ldDecode("ld-decode", "Use this ld-decode for the pass 0 (locating the lines of the captures) instead of doing it here, which gives the same positions.", "program", "ld-decode");
    const QCommandLineOption palOption("pal", "The captures are of a PAL disc (NTSC when not given; --standard pal or secam says it too).");
    const QCommandLineOption rateMhz("rate-mhz", "Capture sample rate in MHz, for pass 0 and when a capture has no .yaml.", "MHz", "75");
    const QCommandLineOption format("format", "Capture format: auto (from the extension), s16, r16, s8, r8, lds, or flac / ldf (decoded with ffmpeg to raw 16-bit samples first).", "format", "auto");
    const QCommandLineOption device("device", "Where the predictor runs: cpu or gpu (OpenCL, the same bits).", "device", "cpu");
    const QCommandLineOption order("order", "Order of coding: auto puts the cleanest capture of each programme first, given keeps the command line order.", "order", "auto");
    const QCommandLineOption flac("flac", "Code the tracks as FLAC instead of with the predictor: the file is then an ordinary Matroska audio file (a FLAC track per capture) that audio programs open, several times larger; unpack gives the captures back all the same.");
    const QCommandLineOption rawOption("raw", "Store the samples as they are instead of coding them: the file is then an ordinary Matroska audio file (a raw PCM track per capture) that audio programs open, as large as the captures; unpack gives the captures back all the same.");
    const QCommandLineOption threads("threads", "Threads coding segments (default: one per processor).", "n");
    DiscOptions disc;
    disc.addTo(parser);
    const QCommandLineOption keepPass0("keep-tbc-curve", "Keep the TBC curve of every capture (worked out from its RF: the lines located, as in the pass 0 of ld-decode) in the file: the speed of the disc and the time base correction are then there for the programs that read it (rf-analyse, ld-decode), without working it out again.");
    for (const QCommandLineOption *o : {&output, &level, &segment, &noVerify, &pass0, &ldDecode, &palOption, &rateMhz, &format, &device, &order, &flac, &rawOption, &threads, &keepPass0}) {
        parser.addOption(*o);
    }
    parser.addOption(sourceOption());
    parser.addOption(assumeLaserdiscOption());
    parser.addPositionalArgument("captures", "The captures to pack.", "<capture>...");
    parser.process(args);
    if (parser.positionalArguments().isEmpty()) {
        return usageError("pack wants at least one capture");
    }
    if (!parser.isSet(output)) {
        return usageError("pack wants --output");
    }
    rfx::Device deviceValue;
    if (!parseDevice(parser.value(device), deviceValue)) {
        return usageError("--device is cpu or gpu");
    }
    rfx::PackOptions options;
    bool ok = false;
    const int levelValue = parser.value(level).toInt(&ok);
    if (!ok || levelValue < 1 || levelValue > 4) {
        return usageError(QStringLiteral("--level is 1 to 4, not '%1'").arg(parser.value(level)));
    }
    options.level = levelValue;
    const qulonglong segmentValue = parser.value(segment).toULongLong(&ok);
    if (!ok || segmentValue == 0) {
        return usageError(QStringLiteral("--segment wants a positive number, not '%1'").arg(parser.value(segment)));
    }
    options.segment = static_cast<size_t>(segmentValue);
    options.verify = !parser.isSet(noVerify);
    options.device = deviceValue;
    if (parser.isSet(pass0)) {
        options.pass0Dir = parser.value(pass0);
    }
    options.ldDecode = parser.value(ldDecode);
    options.externalPass0 = parser.isSet(ldDecode);
    options.pal = parser.isSet(palOption);
    options.keepPass0 = parser.isSet(keepPass0);
    if (const QString bad = readMedium(parser, options); !bad.isEmpty()) {
        return usageError(bad);
    }
    if (const QString bad = disc.read(parser, options.info); !bad.isEmpty()) {
        return usageError(bad);
    }
    options.rateMhz = parser.value(rateMhz).toDouble(&ok);
    if (!ok || options.rateMhz <= 0) {
        return usageError(QStringLiteral("--rate-mhz wants a positive number, not '%1'").arg(parser.value(rateMhz)));
    }
    options.format = parser.value(format).toStdString();
    const QString orderValue = parser.value(order);
    if (orderValue == QLatin1String("auto")) {
        options.order = rfx::OrderMode::Auto;
    } else if (orderValue == QLatin1String("given")) {
        options.order = rfx::OrderMode::Given;
    } else {
        return usageError("--order is auto or given");
    }
    if (parser.isSet(threads)) {
        const uint n = parser.value(threads).toUInt(&ok);
        if (!ok || n == 0) {
            return usageError(QStringLiteral("--threads wants a positive number, not '%1'").arg(parser.value(threads)));
        }
        options.threads = n;
    }
    std::vector<QString> inputs;
    for (const QString &p : parser.positionalArguments()) {
        inputs.push_back(p);
    }
    if (parser.isSet(flac) && parser.isSet(rawOption)) {
        return usageError("--flac and --raw cannot be given together");
    }
    if (parser.isSet(rawOption)) {
        rfx::packFlac(inputs, parser.value(output), options, rfx::StoredCodec::Raw);
        return kExitOk;
    }
    if (parser.isSet(flac)) {
        rfx::packFlac(inputs, parser.value(output), options);
        return kExitOk;
    }
    const std::vector<rfx::PackedTrack> report = rfx::packCaptures(inputs, parser.value(output), options);
    unsigned long long before = 0;
    unsigned long long after = 0;
    for (const rfx::PackedTrack &t : report) {
        before += t.size;
        after += t.packed;
    }
    std::fprintf(stderr, "total: %llu -> %llu bytes (%.1f%%)\n", before, after, 100.0 * static_cast<double>(after) / static_cast<double>(std::max(before, 1ull)));
    return kExitOk;
}

// The options the editing commands share: what they need to code or decode a
// track again.
struct WorkOptions
{
    QCommandLineOption level{{"l", "level"}, "Predictor strength for tracks coded again or added: 1 fastest to 4 strongest.", "n", "3"};
    QCommandLineOption noVerify{"no-verify", "Skip the check that each coded segment decodes back to itself."};
    QCommandLineOption pass0{"pass0", "Pass 0 dumps already made (pass0_<i>.txt, in the order of the tracks written).", "dir"};
    QCommandLineOption ldDecode{"ld-decode", "Use this ld-decode for the pass 0 (locating the lines of the captures) instead of doing it here, which gives the same positions.", "program", "ld-decode"};
    QCommandLineOption palOption{"pal", "The captures are of a PAL disc (NTSC when not given; --standard pal or secam says it too)."};
    QCommandLineOption rateMhz{"rate-mhz", "Capture sample rate in MHz, for pass 0.", "MHz", "75"};
    QCommandLineOption device{"device", "Where the predictor runs: cpu or gpu (OpenCL, the same bits).", "device", "cpu"};
    QCommandLineOption threads{"threads", "Threads coding segments (default: one per processor).", "n"};
    QCommandLineOption keepPass0{"keep-tbc-curve", "Keep the TBC curve of every track (worked out from its RF) in the file: the ones the tracks kept from their files already have are carried over, the others are worked out."};

    void addTo(QCommandLineParser &parser)
    {
        for (const QCommandLineOption *o : {&level, &noVerify, &pass0, &ldDecode, &palOption, &rateMhz, &device, &threads, &keepPass0}) {
            parser.addOption(*o);
        }
        parser.addOption(sourceOption());
        parser.addOption(assumeLaserdiscOption());
    }

    // The options as the packer takes them; an empty string, or the message to
    // give for a value that is not valid.
    QString read(const QCommandLineParser &parser, rfx::PackOptions &out) const
    {
        if (!parseDevice(parser.value(device), out.device)) {
            return QStringLiteral("--device is cpu or gpu");
        }
        bool ok = false;
        const int levelValue = parser.value(level).toInt(&ok);
        if (!ok || levelValue < 1 || levelValue > 4) {
            return QStringLiteral("--level is 1 to 4, not '%1'").arg(parser.value(level));
        }
        out.level = levelValue;
        out.verify = !parser.isSet(noVerify);
        if (parser.isSet(pass0)) {
            out.pass0Dir = parser.value(pass0);
        }
        out.ldDecode = parser.value(ldDecode);
        out.externalPass0 = parser.isSet(ldDecode);
        out.pal = parser.isSet(palOption);
        out.keepPass0 = parser.isSet(keepPass0);
        if (const QString bad = readMedium(parser, out); !bad.isEmpty()) {
            return bad;
        }
        out.rateMhz = parser.value(rateMhz).toDouble(&ok);
        if (!ok || out.rateMhz <= 0) {
            return QStringLiteral("--rate-mhz wants a positive number, not '%1'").arg(parser.value(rateMhz));
        }
        if (parser.isSet(threads)) {
            const uint n = parser.value(threads).toUInt(&ok);
            if (!ok || n == 0) {
                return QStringLiteral("--threads wants a positive number, not '%1'").arg(parser.value(threads));
            }
            out.threads = n;
        }
        return QString();
    }
};

// A list of track numbers given as --option 0,3,5 (the option may be repeated).
bool trackList(const QCommandLineParser &parser, const QCommandLineOption &option, std::vector<size_t> &out, QString &error)
{
    for (const QString &value : parser.values(option)) {
        for (const QString &item : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            bool ok = false;
            const uint n = item.trimmed().toUInt(&ok);
            if (!ok) {
                error = QStringLiteral("--%1 wants track numbers, not '%2'").arg(option.names().last(), item);
                return false;
            }
            out.push_back(n);
        }
    }
    return true;
}

// An .rfx file that may carry the tracks asked for, "file.rfx:0,2" (no list: all of them). The list is taken only when what follows the last colon
// is numbers, so that "C:\disc.rfx" is a path.
struct ArchiveArg
{
    QString path;
    std::vector<size_t> tracks;
};

ArchiveArg splitArchiveArg(const QString &arg)
{
    ArchiveArg out;
    out.path = arg;
    const qsizetype colon = arg.lastIndexOf(QLatin1Char(':'));
    if (colon > 1) {
        const QStringList parts = arg.mid(colon + 1).split(QLatin1Char(','));
        std::vector<size_t> tracks;
        bool all = !parts.isEmpty();
        for (const QString &p : parts) {
            bool ok = false;
            const uint v = p.toUInt(&ok);
            all = all && ok;
            tracks.push_back(v);
        }
        if (all) {
            out.path = arg.left(colon);
            out.tracks = std::move(tracks);
        }
    }
    return out;
}

// The picks of the tracks of one of the archives (all of them, or the ones named), for the archive number `arch`.
std::vector<rfx::Pick> picksOfArchive(const ArchiveArg &a, size_t arch)
{
    std::vector<rfx::Pick> picks = a.tracks.empty() ? rfx::pickAll({a.path}) : rfx::pickSome(a.path, a.tracks);
    for (rfx::Pick &p : picks) {
        p.arch = arch;
    }
    return picks;
}

int runEdit(const QString &command, const QStringList &args)
{
    QCommandLineParser parser;
    WorkOptions work;
    const QCommandLineOption output({"o", "output"}, "The .rfx file to write (a new file).", "file");
    const QCommandLineOption recode("recode", "Code the tracks of the later files again against everything before them, so that captures of one disc in different files predict each other.");
    const QCommandLineOption only("only", "Tracks to keep (0 is the primary), e.g. 0,3,5.", "tracks");
    const QCommandLineOption track("track", "Tracks to remove (0 is the primary), e.g. 2,4.", "tracks");
    parser.addHelpOption();
    parser.addOption(output);
    work.addTo(parser);
    DiscOptions disc;
    disc.addTo(parser);
    const QCommandLineOption changed("track", "The tracks to say it of (0 is the primary), e.g. 1,3; all of them when not given.", "tracks");
    if (command == QLatin1String("merge")) {
        parser.setApplicationDescription("Merge .rfx files into one (the tracks of each after the ones of the previous; no track is coded again unless --recode is given). Some tracks of a file only: file.rfx:0,2 (0 is the primary); without a list, all of them.");
        parser.addOption(recode);
        parser.addPositionalArgument("archives", "The .rfx files to merge, each with the tracks to take if not all: file.rfx:0,2.", "<archive>[:tracks] <archive>[:tracks]...");
    } else if (command == QLatin1String("extract")) {
        parser.setApplicationDescription("Write some of the tracks of a .rfx file as a .rfx file of their own.");
        parser.addOption(only);
        parser.addPositionalArgument("archive", "The .rfx file.");
    } else if (command == QLatin1String("remove")) {
        parser.setApplicationDescription("Write a .rfx file without some of the tracks of another.");
        parser.addOption(track);
        parser.addPositionalArgument("archive", "The .rfx file.");
    } else if (command == QLatin1String("tag")) {
        parser.setApplicationDescription("Write a .rfx file with what is said of the disc and the signal changed in some tracks of another (--standard, --reference, --copy, --face, --signal): nothing is coded again, the samples are copied as they are.");
        parser.addOption(changed);
        parser.addPositionalArgument("archive", "The .rfx file.");
    } else {
        parser.setApplicationDescription("Write a .rfx file with more tracks than another: captures, and the tracks of other .rfx files (copied as they are where they can be; some of them only: other.rfx:1,3).");
        parser.addPositionalArgument("archive", "The .rfx file.");
        parser.addPositionalArgument("captures", "Captures to add, in any format pack takes.", "<capture>...");
    }
    parser.process(args);
    if (!parser.isSet(output)) {
        return usageError(command + " wants --output");
    }
    rfx::PackOptions options;
    const QString bad = work.read(parser, options);
    if (!bad.isEmpty()) {
        return usageError(bad);
    }
    rfx::InfoEdit info;
    if (const QString bad = disc.read(parser, info); !bad.isEmpty()) {
        return usageError(bad);
    }
    options.info = info;
    const auto say = [&info](std::vector<rfx::Pick> &picks) {
        // (said of every track that is written; of the new captures it is also what the options say)
        for (rfx::Pick &p : picks) {
            p.edit.info = info;
        }
    };
    const QStringList positional = parser.positionalArguments();
    const QString out = parser.value(output);
    QString error;
    if (command == QLatin1String("merge")) {
        if (positional.size() < 2) {
            return usageError("merge wants at least two .rfx files");
        }
        std::vector<QString> archives;
        std::vector<rfx::Pick> picks;
        for (const QString &arg : positional) {
            const ArchiveArg a = splitArchiveArg(arg);
            archives.push_back(a.path);
            for (rfx::Pick &p : picksOfArchive(a, archives.size() - 1)) {
                picks.push_back(std::move(p));
            }
        }
        if (picks.empty()) {
            return usageError("merge has no track to write");
        }
        say(picks);
        rfx::assembleArchives(archives, picks, out, options, parser.isSet(recode) ? std::optional<size_t>(1) : std::nullopt);
    } else if (command == QLatin1String("extract") || command == QLatin1String("remove")) {
        if (positional.size() != 1) {
            return usageError(command + " wants one .rfx file");
        }
        const bool extract = command == QLatin1String("extract");
        std::vector<size_t> which;
        if (!trackList(parser, extract ? only : track, which, error)) {
            return usageError(error);
        }
        if (which.empty()) {
            return usageError(command + (extract ? " wants --only" : " wants --track"));
        }
        std::vector<rfx::Pick> picks = extract ? rfx::pickSome(positional[0], which) : rfx::pickWithout(positional[0], which);
        say(picks);
        rfx::assembleArchives({positional[0]}, picks, out, options, std::nullopt);
    } else if (command == QLatin1String("tag")) {
        if (positional.size() != 1) {
            return usageError("tag wants one .rfx file");
        }
        if (!info.any() && !options.medium) {
            return usageError("tag wants something to say: --source, --standard, --reference, --copy, --capture-number, --face or --signal");
        }
        std::vector<size_t> which;
        if (!trackList(parser, changed, which, error)) {
            return usageError(error);
        }
        std::vector<rfx::Pick> picks = rfx::pickAll({positional[0]});
        for (const size_t t : which) {
            if (t >= picks.size()) {
                return usageError(QStringLiteral("the file has %1 tracks, there is no track %2").arg(picks.size()).arg(t));
            }
        }
        for (size_t i = 0; i < picks.size(); ++i) {
            if (which.empty() || std::find(which.begin(), which.end(), i) != which.end()) {
                picks[i].edit.info = info;
            }
        }
        rfx::assembleArchives({positional[0]}, picks, out, options, std::nullopt);
    } else {
        if (positional.size() < 2) {
            return usageError("add wants a .rfx file and at least one capture or .rfx file to add");
        }
        // What follows the file is added in the order it is given: the captures, and the tracks of other .rfx files (copied as they are where they can be).
        std::vector<QString> archives = {positional[0]};
        std::vector<rfx::Pick> picks = rfx::pickAll({positional[0]});
        for (int i = 1; i < positional.size(); ++i) {
            const ArchiveArg a = splitArchiveArg(positional[i]);
            if (a.path.endsWith(QLatin1String(".rfx"), Qt::CaseInsensitive)) {
                archives.push_back(a.path);
                for (rfx::Pick &p : picksOfArchive(a, archives.size() - 1)) {
                    picks.push_back(std::move(p));
                }
            } else {
                picks.push_back(rfx::Pick::fresh(positional[i]));
            }
        }
        say(picks);
        rfx::assembleArchives(archives, picks, out, options, std::nullopt);
    }
    return kExitOk;
}

// A positive number option; false (with the message) if it is not one.
bool positiveOption(const QCommandLineParser &parser, const QCommandLineOption &option, size_t &out, QString &error)
{
    bool ok = false;
    const qulonglong v = parser.value(option).toULongLong(&ok);
    if (!ok || v == 0) {
        error = QStringLiteral("--%1 wants a positive number, not '%2'").arg(option.names().last(), parser.value(option));
        return false;
    }
    out = static_cast<size_t>(v);
    return true;
}

// Development: pass 0 on captures, with no ld-decode: the dumps ld-decode writes with LD_DUMP_PASS0, to compare.
int runPass0Command(const QStringList &args)
{
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.setApplicationDescription("Development: locate the lines of the captures from their RF (the pass 0 of ld-decode) and write pass0_<i>.txt.");
    const QCommandLineOption outdir({"o", "output"}, "Folder for the dumps.", "dir");
    const QCommandLineOption rate("rate-mhz", "Sample rate in MHz (default: from the .yaml).", "MHz");
    const QCommandLineOption format("format", "Capture format: s16, r16, s8, r8 or lds (from the extension when not given).", "format");
    const QCommandLineOption pal("pal", "A PAL disc (default NTSC).");
    const QCommandLineOption threads("threads", "Threads (default: one per processor).", "n");
    for (const QCommandLineOption *o : {&outdir, &rate, &format, &pal, &threads}) {
        parser.addOption(*o);
    }
    parser.addPositionalArgument("captures", "The captures.", "<capture>...");
    parser.process(args);
    if (parser.positionalArguments().isEmpty() || !parser.isSet(outdir)) {
        return usageError("pass0 wants captures and --output");
    }
    QDir().mkpath(parser.value(outdir));
    const unsigned threadCount = parser.isSet(threads) ? parser.value(threads).toUInt() : 0;
    size_t i = 0;
    for (const QString &path : parser.positionalArguments()) {
        rfx::CaptureSpec spec;
        const rfx::SampleFormat fmt = rfx::inputFormat(path, parser.isSet(format) ? parser.value(format).toStdString() : "auto");
        spec.format = fmt;
        std::optional<double> hz = rfx::sidecarRateHz(rfx::findSidecars(path));
        if (parser.isSet(rate)) {
            hz = parser.value(rate).toDouble() * 1e6;
        }
        if (!hz) {
            return usageError("the rate of " + path + " is not known: give --rate-mhz");
        }
        spec.rateHz = *hz;
        spec.bits = 16;
        rfx::TrackReader reader(path, spec);
        const std::vector<rfx::Field> fields = rfx::locateFields(reader, 0, *hz, parser.isSet(pal) ? rfx::VideoSystem::Pal : rfx::VideoSystem::Ntsc, threadCount);
        const std::string text = rfx::formatPass0(fields);
        QFile out(QDir(parser.value(outdir)).filePath(QStringLiteral("pass0_%1.txt").arg(i)));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size())) {
            throw rfx::Error("cannot write " + out.fileName().toStdString());
        }
        std::fprintf(stderr, "%s: %zu fields\n", path.toLocal8Bit().constData(), fields.size());
        ++i;
    }
    return kExitOk;
}

// Development: noise, refsel, bench, gpu-check.
int runDiagnostic(const QString &command, const QStringList &args)
{
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption pass0("pass0", "Pass 0 dumps (pass0_<i>.txt, in the order of the captures).", "dir");
    const QCommandLineOption windows("windows", "Windows measured on each capture.", "n", "8");
    const QCommandLineOption window("window", "Samples in a window.", "n", "262144");
    const QCommandLineOption target("target", "Coding place of the capture (0 is the primary).", "n");
    const QCommandLineOption level({"l", "level"}, "Predictor strength: 1 fastest to 4 strongest.", "n", "3");
    const QCommandLineOption segments("segments", "Segments looked at.", "n");
    const QCommandLineOption segment("segment", "Samples in a segment.", "n", "262144");
    const QCommandLineOption withRef("with-ref", "Predict from a (synthetic) reference window too.");
    const QCommandLineOption threads("threads", "Threads (default: one per processor).", "n");
    if (command == QLatin1String("noise")) {
        parser.setApplicationDescription("Development: how noisy each capture is (the cost of coding it alone, on the same frames).");
        parser.addOption(pass0);
        parser.addOption(windows);
        parser.addOption(window);
        parser.addOption(threads);
        parser.addPositionalArgument("captures", "The captures, in the order of the dumps.", "<capture>...");
    } else if (command == QLatin1String("refsel")) {
        parser.setApplicationDescription("Development: would other references have cost less for one capture?");
        parser.addOption(target);
        parser.addOption(pass0);
        parser.addOption(segments);
        parser.addOption(threads);
        parser.addPositionalArgument("captures", "The captures, in the order of the dumps.", "<capture>...");
    } else if (command == QLatin1String("bench")) {
        parser.setApplicationDescription("Development: single-thread speed of each stage, on a 16-bit capture.");
        parser.addOption(level);
        parser.addOption(segments);
        parser.addOption(segment);
        parser.addPositionalArgument("capture", "A 16-bit capture.");
    } else {
        parser.setApplicationDescription("Development: the predictor on the CPU and on the GPU, on a 16-bit capture.");
        parser.addOption(withRef);
        parser.addOption(level);
        parser.addOption(segments);
        parser.addOption(segment);
        parser.addPositionalArgument("capture", "A 16-bit capture.");
    }
    parser.process(args);
    const QStringList positional = parser.positionalArguments();
    QString error;
    unsigned threadCount = 0;
    if (parser.isSet(threads)) {
        bool ok = false;
        threadCount = parser.value(threads).toUInt(&ok);
        if (!ok || threadCount == 0) {
            return usageError(QStringLiteral("--threads wants a positive number, not '%1'").arg(parser.value(threads)));
        }
    }
    int levelValue = 3;
    if (command == QLatin1String("bench") || command == QLatin1String("gpu-check")) {
        bool ok = false;
        levelValue = parser.value(level).toInt(&ok);
        if (!ok || levelValue < 1 || levelValue > 4) {
            return usageError(QStringLiteral("--level is 1 to 4, not '%1'").arg(parser.value(level)));
        }
        if (positional.size() != 1) {
            return usageError(command + " wants one capture");
        }
    } else {
        if (positional.isEmpty()) {
            return usageError(command + " wants captures");
        }
        if (!parser.isSet(pass0)) {
            return usageError(command + " wants --pass0");
        }
    }
    size_t count = 0;
    size_t length = 0;
    std::vector<QString> inputs(positional.begin(), positional.end());
    if (command == QLatin1String("noise")) {
        if (!positiveOption(parser, windows, count, error) || !positiveOption(parser, window, length, error)) {
            return usageError(error);
        }
        rfx::printNoise(inputs, parser.value(pass0), count, length, threadCount);
    } else if (command == QLatin1String("refsel")) {
        if (!parser.isSet(target)) {
            return usageError("refsel wants --target");
        }
        bool ok = false;
        const qulonglong place = parser.value(target).toULongLong(&ok);
        if (!ok) {
            return usageError(QStringLiteral("--target wants a place, not '%1'").arg(parser.value(target)));
        }
        count = 12;
        if (parser.isSet(segments) && !positiveOption(parser, segments, count, error)) {
            return usageError(error);
        }
        rfx::PackOptions options;
        options.pass0Dir = parser.value(pass0);
        options.verify = false;
        options.threads = threadCount;
        rfx::printReferenceChoice(inputs, options, static_cast<size_t>(place), count);
    } else {
        count = command == QLatin1String("bench") ? 16 : 8;
        if (parser.isSet(segments) && !positiveOption(parser, segments, count, error)) {
            return usageError(error);
        }
        if (!positiveOption(parser, segment, length, error)) {
            return usageError(error);
        }
        if (command == QLatin1String("bench")) {
            rfx::printSpeed(positional[0], levelValue, count, length);
        } else {
            rfx::printGpuCheck(positional[0], levelValue, count, length, parser.isSet(withRef));
        }
    }
    return kExitOk;
}

void printHelp()
{
    std::fputs(
        "Usage: rfx <command> [options] <file>\n"
        "\n"
        "Several RF captures of one disc in one lossless .rfx file.\n"
        "\n"
        "Commands:\n"
        "  pack      Pack captures into one .rfx file\n"
        "  merge     Merge .rfx files into one\n"
        "  extract   Write some of the tracks as a .rfx file of their own\n"
        "  remove    Write a .rfx file without some of the tracks\n"
        "  add       Write a .rfx file with more captures or the tracks of other .rfx files\n"
        "  tag       Write a .rfx file with the source of the file, or the standard, reference, copy, capture number, face or signal of some tracks, said or changed\n"
        "  tbc-curve Work out the TBC curve of the tracks from their RF and keep it in the file (the speed, the time base correction)\n"
        "  check     Check the integrity of .rfx files: every track against its SHA-256 (nothing is written)\n"
        "  info      Show what an .rfx file holds (--yaml: the whole metadata)\n"
        "  unpack    Rebuild the original files, checked by SHA-256 (--meta: metadata only)\n"
        "\n"
        "Development:\n"
        "  noise     How noisy each capture is (the cost of coding it alone, on the same frames)\n"
        "  refsel    Would other references have cost less for one capture?\n"
        "  bench     Single-thread speed of each stage, on a 16-bit capture\n"
        "  pass0     Locate the lines of captures from their RF, as ld-decode's pass 0 does (writes its dumps)\n"
        "  gpu-check The predictor on the CPU and on the GPU, on a 16-bit capture\n"
        "\n"
        "Run 'rfx <command> --help' for the options of a command.\n",
        stdout);
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // The output is the same bytes on every system: a line feed stays one.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("rfx");
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION " (" APP_BRANCH " " APP_COMMIT ")"));

    QStringList args = app.arguments();
    if (args.size() < 2) {
        printHelp();
        return kExitUsage;
    }
    const QString command = args.at(1);
    if (command == QLatin1String("--help") || command == QLatin1String("-h")) {
        printHelp();
        return kExitOk;
    }
    if (command == QLatin1String("--version") || command == QLatin1String("-v")) {
        std::printf("rfx %s\n", QCoreApplication::applicationVersion().toStdString().c_str());
        return kExitOk;
    }
    // The command is the program's own: the parser of the command sees "rfx <command>" as its program name.
    QStringList rest = args.mid(1);
    rest[0] = args.at(0) + QLatin1Char(' ') + command;
    try {
        if (command == QLatin1String("pack")) {
            return runPack(rest);
        }
        if (command == QLatin1String("merge") || command == QLatin1String("extract") || command == QLatin1String("remove") || command == QLatin1String("add") || command == QLatin1String("tag")) {
            return runEdit(command, rest);
        }
        if (command == QLatin1String("noise") || command == QLatin1String("refsel") || command == QLatin1String("bench") || command == QLatin1String("gpu-check")) {
            return runDiagnostic(command, rest);
        }
        if (command == QLatin1String("pass0")) {
            return runPass0Command(rest);
        }
        if (command == QLatin1String("tbc-curve")) {
            return runKeepPass0(rest);
        }
        if (command == QLatin1String("check")) {
            return runCheck(rest);
        }
        if (command == QLatin1String("info")) {
            return runInfo(rest);
        }
        if (command == QLatin1String("unpack")) {
            return runUnpack(rest);
        }
    } catch (const rfx::Error &e) {
        std::fprintf(stderr, "rfx: %s\n", e.what());
        return kExitFailed;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "rfx: %s\n", e.what());
        return kExitFailed;
    }
    return usageError(QStringLiteral("unknown command '%1' (pack, merge, extract, remove, add, tag, tbc-curve, check, info, unpack, noise, refsel, bench, gpu-check)").arg(command));
}
