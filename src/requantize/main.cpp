/******************************************************************************
 * main.cpp
 * rf-requantize - requantise a capture or an .rfx file as the capture
 * application would have, to as few bits as the noise of the capture allows
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools. It is ddd-requantize of the Domesday
 * Duplicator, ported, which reads and writes .rfx files as well as captures.
 ******************************************************************************/

#include "error.h"
#include "requantize.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

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
    std::fprintf(stderr, "rf-requantize: %s\n", message.toStdString().c_str());
    return kExitUsage;
}

// A whole number in [low, high]; false when the text is not one.
bool whole(const QString &text, int low, int high, int &out)
{
    bool ok = false;
    const int value = text.trimmed().toInt(&ok);
    if (!ok || value < low || value > high) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // Samples, not text: a 0x0D 0x0A pair inside a sample must stay two bytes
    // whichever way they go.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("rf-requantize");
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION " (" APP_BRANCH " " APP_COMMIT ")"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Requantise a capture, or the tracks of an .rfx file, as the capture application would have: each segment of about 35 ms is\n"
        "cut to as few bits as the noise of the capture allows, and the bits cut cost nothing in a FLAC or .rfx file. The samples\n"
        "are those the Domesday Duplicator's capture application writes with the same settings. Capture once with requantisation\n"
        "off, then requantise that one capture each way worth trying, and compare the files.\n"
        "\n"
        "The tool does not change formats: what comes out is coded as what went in. It needs no ld-decode; ffmpeg only for FLAC.\n"
        "\n"
        "  <input>   a capture (.s16 .r16 .s8 .r8 .lds, or .flac / .ldf), an .rfx file of either variant, or - for signed 16-bit\n"
        "            little-endian samples on standard input\n"
        "  <output>  the same: an .rfx file for an .rfx file (the tracks coded again as they were, lag tables kept), a .flac for\n"
        "            a .flac, a raw capture of the input's format for a raw one, or - for signed 16-bit samples on standard\n"
        "            output. Without one nothing is written, and the report says what a capture would have done.\n"
        "\n"
        "Exit codes: 0 success, 1 the work failed, 2 the command line is wrong.");
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption rate("rate", "The samples' rate. A track of an .rfx file says what it is, and so can a .yaml beside a capture; anything else has to be told: 30 for 60 MHz halved.", "MHz");
    const QCommandLineOption margin("margin", "How far the noise floor may rise in any 1 MHz slice of the protected bands: 0 aggressive (1 dB), 1 (0.5), 2 safe (0.2, the default), 3 (0.1), 4 (0.05). off writes the samples as they are: the reference to compare the others with.", "0-4|off", "2");
    const QCommandLineOption band({"band", "bands"}, "The protected bands in MHz: 0-12, 2-14, or several, 0-1.9,2.1-13.5. Default: DC to 14 MHz, or 1.5 MHz short of the Nyquist limit.", "bands");
    const QCommandLineOption shaping("shaping", "fixed (the default) or adaptive.", "shaping", "fixed");
    const QCommandLineOption depth("depth", "How far above the bands the shaped noise may go, 0 to 40: one depth for everywhere outside them, or one for each stretch between them from DC up (with bands 2-14, 10,40 spares the EFM and puts the rest above 14 MHz). Default 10 fixed, 20 adaptive.", "dB[,dB...]");
    const QCommandLineOption order("order", "The shaping filter's order, 2 to 64. Default 16 fixed, 32 adaptive.", "n");
    const QCommandLineOption inputBits("input-bits", "The bits the input carries, 5 to 16. A track of an .rfx file says; otherwise they are found from the samples.", "n");
    const QCommandLineOption format("format", "Format of the input capture: auto (from the extension), s16, r16, s8, r8, lds, or flac / ldf.", "format", "auto");
    const QCommandLineOption only("only", "Tracks of an .rfx input to take (0 is the primary), e.g. 0,3,5.", "tracks");
    const QCommandLineOption compression("compression", "FLAC level for a .flac or .ldf output, 0 to 8. Default 8.", "0-8", "8");
    const QCommandLineOption log("log", "One line per segment: what was decided, and the slice that held it back (CSV).", "file.csv");
    const QCommandLineOption realtime("realtime", "Hand the samples on no faster than their rate, as a capture does, for a decoder downstream that expects a live stream.");
    // An .rfx output is coded as its input was: only how it is run is chosen.
    const QCommandLineOption noVerify("no-verify", "Skip the check that each segment of an .rfx output decodes back to itself.");
    const QCommandLineOption device("device", "Where the predictor runs: cpu or gpu (OpenCL, the same bits).", "device", "cpu");
    const QCommandLineOption threads("threads", "Threads (default: one per processor).", "n");
    for (const QCommandLineOption *o : {&rate, &margin, &band, &shaping, &depth, &order, &inputBits, &format, &only, &compression, &log, &realtime, &noVerify, &device, &threads}) {
        parser.addOption(*o);
    }
    parser.addPositionalArgument("input", "The capture or .rfx file to requantise.");
    parser.addPositionalArgument("output", "What to write.", "[<output>]");

    if (!parser.parse(QCoreApplication::arguments())) {
        return usageError(parser.errorText());
    }
    if (parser.isSet(QStringLiteral("version"))) {
        parser.showVersion();
    }
    if (parser.isSet(QStringLiteral("help"))) {
        parser.showHelp(kExitOk);
    }
    const QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        return usageError("No capture was given to requantise.");
    }
    if (positional.size() > 2) {
        return usageError("One capture in and at most one out.");
    }

    rfx::RequantizeJob job;
    job.input = positional.at(0);
    if (positional.size() == 2) {
        job.output = positional.at(1);
    }
    job.format = parser.value(format).toStdString();

    bool ok = false;
    if (parser.isSet(rate)) {
        const double mhz = parser.value(rate).toDouble(&ok);
        if (!ok || !(mhz > 0.0)) {
            return usageError(QStringLiteral("Unknown --rate '%1'. Give the samples' rate in MHz: 30, or 32.5.").arg(parser.value(rate)));
        }
        job.rateMhz = mhz;
    }

    rfx::RequantizeOptions &options = job.options;
    const QString marginValue = parser.value(margin);
    if (marginValue == QLatin1String("off")) {
        options.requantize = false;
    } else if (!whole(marginValue, rfx::kMinimumMarginLevel, rfx::kMaximumMarginLevel, options.marginLevel)) {
        return usageError(QStringLiteral("Unknown --margin '%1'. Use 0 to 4, or off.").arg(marginValue));
    }
    if (parser.isSet(band)) {
        options.bands = rfx::parseBands(parser.value(band).toStdString());
        if (options.bands.empty()) {
            return usageError(QStringLiteral("Unknown --band '%1'. Give bands in MHz, low to high: 0-12, or 0-1.9,2.1-13.5.").arg(parser.value(band)));
        }
    }
    const QString shapingValue = parser.value(shaping);
    if (shapingValue == QLatin1String("adaptive")) {
        options.adaptive = true;
    } else if (shapingValue != QLatin1String("fixed")) {
        return usageError(QStringLiteral("Unknown --shaping '%1'. Use fixed or adaptive.").arg(shapingValue));
    }
    if (parser.isSet(depth)) {
        options.depthsDb = rfx::parseShapingDepths(parser.value(depth).toStdString());
        if (options.depthsDb.empty()) {
            return usageError(QStringLiteral("Unknown --depth '%1'. Give a depth in dB, 0 to 40, or one for each stretch between the bands: 10,40.").arg(parser.value(depth)));
        }
    }
    if (parser.isSet(order) && !whole(parser.value(order), rfx::kMinimumShapingOrder, rfx::kMaximumShapingOrder, options.order)) {
        return usageError(QStringLiteral("Unknown --order '%1'. Use 2 to 64.").arg(parser.value(order)));
    }
    if (parser.isSet(inputBits)) {
        int bits = 0;
        if (!whole(parser.value(inputBits), rfx::kFewestInputBits, rfx::kMostInputBits, bits)) {
            return usageError(QStringLiteral("Unknown --input-bits '%1'. Use 5 to 16.").arg(parser.value(inputBits)));
        }
        options.inputBits = bits;
    }
    options.realtime = parser.isSet(realtime);
    if (!whole(parser.value(compression), 0, 8, job.flacCompression)) {
        return usageError(QStringLiteral("Unknown --compression '%1'. Use 0 to 8.").arg(parser.value(compression)));
    }
    if (parser.isSet(log)) {
        if (parser.value(log).isEmpty()) {
            return usageError("--log needs a file to write to.");
        }
        job.logPath = parser.value(log);
    }
    for (const QString &value : parser.values(only)) {
        for (const QString &item : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const uint track = item.trimmed().toUInt(&ok);
            if (!ok) {
                return usageError(QStringLiteral("--only wants track numbers, not '%1'").arg(item));
            }
            job.only.push_back(track);
        }
    }

    job.pack.verify = !parser.isSet(noVerify);
    const QString deviceValue = parser.value(device);
    if (deviceValue == QLatin1String("gpu")) {
        job.pack.device = rfx::Device::Gpu;
    } else if (deviceValue != QLatin1String("cpu")) {
        return usageError("--device is cpu or gpu");
    }
    if (parser.isSet(threads)) {
        const uint n = parser.value(threads).toUInt(&ok);
        if (!ok || n == 0) {
            return usageError(QStringLiteral("--threads wants a positive number, not '%1'").arg(parser.value(threads)));
        }
        job.pack.threads = n;
    }

    // Nothing but samples reaches standard output when it is carrying them.
    job.standardInput = stdin;
    job.standardOutput = stdout;
    job.report = job.output == QLatin1String("-") ? stderr : stdout;
    job.showProgress = true;

    try {
        rfx::runRequantize(job);
    } catch (const rfx::UsageError &e) {
        return usageError(QString::fromUtf8(e.what()));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "rf-requantize: %s\n", e.what());
        return kExitFailed;
    }
    return kExitOk;
}
