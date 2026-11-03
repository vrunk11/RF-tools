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
#include "edit.h"
#include "error.h"
#include "flacmode.h"
#include "packer.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
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

int runUnpack(const QStringList &args)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Rebuild the original files of an .rfx file, checking each against its SHA-256.");
    parser.addHelpOption();
    const QCommandLineOption outdir({"d", "outdir"}, "Directory to write into (default: the current one).", "dir", ".");
    const QCommandLineOption only("only", "Extract only this track (0 is the primary).", "track");
    const QCommandLineOption meta("meta", "Write only the metadata (each track's .yaml as it was, and <archive>.yaml with all of it); no sample is decoded.");
    const QCommandLineOption device("device", "Where the predictor runs: cpu (the only one the C++ tools have).", "device", "cpu");
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
    if (parser.value(device) != QLatin1String("cpu")) {
        return usageError("--device must be cpu: the C++ tools have no GPU predictor");
    }
    rfx::UnpackOptions options;
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
    } else if (flac) {
        if (options.target && options.target->kind == rfx::OutputFormat::Kind::MultichannelFlac) {
            throw rfx::Error("--as: unknown format mflac for a FLAC-mode archive");
        }
        rfx::unpackFlac(path, dir, options.only, options.target);
    } else if (options.target && options.target->kind == rfx::OutputFormat::Kind::MultichannelFlac) {
        rfx::exportMultichannelFlac(path, dir, options.only);
    } else {
        rfx::unpackArchive(path, dir, options);
    }
    return kExitOk;
}

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
    const QCommandLineOption ldDecode("ld-decode", "ld-decode, to locate the lines of the captures (pass 0).", "program", "ld-decode");
    const QCommandLineOption rateMhz("rate-mhz", "Capture sample rate in MHz, for pass 0 and when a capture has no .yaml.", "MHz", "75");
    const QCommandLineOption format("format", "Capture format: auto (from the extension), s16, r16, s8, r8, lds, or flac / ldf (decoded with ffmpeg to raw 16-bit samples first).", "format", "auto");
    const QCommandLineOption device("device", "Where the predictor runs: cpu (the only one the C++ tools have).", "device", "cpu");
    const QCommandLineOption order("order", "Order of coding: auto puts the cleanest capture of each programme first, given keeps the command line order.", "order", "auto");
    const QCommandLineOption flac("flac", "Code the tracks as FLAC instead of with the predictor: the file is then an ordinary Matroska audio file (a FLAC track per capture) that audio programs open, several times larger; unpack gives the captures back all the same.");
    const QCommandLineOption threads("threads", "Threads coding segments (default: one per processor).", "n");
    for (const QCommandLineOption *o : {&output, &level, &segment, &noVerify, &pass0, &ldDecode, &rateMhz, &format, &device, &order, &flac, &threads}) {
        parser.addOption(*o);
    }
    parser.addPositionalArgument("captures", "The captures to pack.", "<capture>...");
    parser.process(args);
    if (parser.positionalArguments().isEmpty()) {
        return usageError("pack wants at least one capture");
    }
    if (!parser.isSet(output)) {
        return usageError("pack wants --output");
    }
    if (parser.value(device) != QLatin1String("cpu")) {
        return usageError("--device must be cpu: the C++ tools have no GPU predictor");
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
    if (parser.isSet(pass0)) {
        options.pass0Dir = parser.value(pass0);
    }
    options.ldDecode = parser.value(ldDecode);
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
    QCommandLineOption ldDecode{"ld-decode", "ld-decode, to locate the lines of the captures (pass 0).", "program", "ld-decode"};
    QCommandLineOption rateMhz{"rate-mhz", "Capture sample rate in MHz, for pass 0.", "MHz", "75"};
    QCommandLineOption device{"device", "Where the predictor runs: cpu (the only one the C++ tools have).", "device", "cpu"};
    QCommandLineOption threads{"threads", "Threads coding segments (default: one per processor).", "n"};

    void addTo(QCommandLineParser &parser)
    {
        for (const QCommandLineOption *o : {&level, &noVerify, &pass0, &ldDecode, &rateMhz, &device, &threads}) {
            parser.addOption(*o);
        }
    }

    // The options as the packer takes them; an empty string, or the message to
    // give for a value that is not valid.
    QString read(const QCommandLineParser &parser, rfx::PackOptions &out) const
    {
        if (parser.value(device) != QLatin1String("cpu")) {
            return QStringLiteral("--device must be cpu: the C++ tools have no GPU predictor");
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
    if (command == QLatin1String("merge")) {
        parser.setApplicationDescription("Merge .rfx files into one (the tracks of each after the ones of the previous; no track is coded again unless --recode is given).");
        parser.addOption(recode);
        parser.addPositionalArgument("archives", "The .rfx files to merge.", "<archive> <archive>...");
    } else if (command == QLatin1String("extract")) {
        parser.setApplicationDescription("Write some of the tracks of a .rfx file as a .rfx file of their own.");
        parser.addOption(only);
        parser.addPositionalArgument("archive", "The .rfx file.");
    } else if (command == QLatin1String("remove")) {
        parser.setApplicationDescription("Write a .rfx file without some of the tracks of another.");
        parser.addOption(track);
        parser.addPositionalArgument("archive", "The .rfx file.");
    } else {
        parser.setApplicationDescription("Write a .rfx file with more captures than another.");
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
    const QStringList positional = parser.positionalArguments();
    const QString out = parser.value(output);
    QString error;
    if (command == QLatin1String("merge")) {
        if (positional.size() < 2) {
            return usageError("merge wants at least two .rfx files");
        }
        std::vector<QString> archives(positional.begin(), positional.end());
        rfx::assembleArchives(archives, rfx::pickAll(archives), out, options, parser.isSet(recode) ? std::optional<size_t>(1) : std::nullopt);
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
        const std::vector<rfx::Pick> picks = extract ? rfx::pickSome(positional[0], which) : rfx::pickWithout(positional[0], which);
        rfx::assembleArchives({positional[0]}, picks, out, options, std::nullopt);
    } else {
        if (positional.size() < 2) {
            return usageError("add wants a .rfx file and at least one capture");
        }
        std::vector<rfx::Pick> picks = rfx::pickAll({positional[0]});
        for (int i = 1; i < positional.size(); ++i) {
            picks.push_back(rfx::Pick::fresh(positional[i]));
        }
        rfx::assembleArchives({positional[0]}, picks, out, options, std::nullopt);
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
        "  add       Write a .rfx file with more captures\n"
        "  info      Show what an .rfx file holds (--yaml: the whole metadata)\n"
        "  unpack    Rebuild the original files, checked by SHA-256 (--meta: metadata only)\n"
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
        if (command == QLatin1String("merge") || command == QLatin1String("extract") || command == QLatin1String("remove") || command == QLatin1String("add")) {
            return runEdit(command, rest);
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
    return usageError(QStringLiteral("unknown command '%1' (pack, merge, extract, remove, add, info, unpack)").arg(command));
}
