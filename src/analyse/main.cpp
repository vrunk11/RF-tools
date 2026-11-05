/******************************************************************************
 * main.cpp
 * rf-analyse - look at the RF captures stored in .rfx files, or at raw captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "capture.h"
#include "capturedialog.h"
#include "mainwindow.h"
#include "trackreader.h"

#include <QApplication>
#include <QIcon>

#include <algorithm>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>

#include <cstdio>
#include <string>

namespace {

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kFewestBits = 1;
constexpr int kMostBits = 16;

int usage(const std::string &message)
{
    std::fprintf(stderr, "rf-analyse: %s\n", message.c_str());
    return kExitUsage;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("rf-analyse"));
    Q_INIT_RESOURCE(rf_analyse_resources);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/Graphics/256-rf-analyse.png")));
    QCoreApplication::setOrganizationName(QStringLiteral("RF-tools"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION " (" APP_BRANCH " " APP_COMMIT ")"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Look at the RF captures stored in .rfx files, or at raw captures: the waveform, the spectrum, the noise and the drift of each track. Several files open together, their tracks one after the other."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption view(QStringLiteral("view"), QStringLiteral("The view to start with: combined, individual (the selected track) or lanes."), QStringLiteral("view"),
                                  QStringLiteral("combined"));
    const QCommandLineOption screenshot(QStringLiteral("screenshot"), QStringLiteral("Write what the window shows to a PNG file and quit (for documentation and tests)."),
                                        QStringLiteral("file.png"));
    const QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Size of the window, as 1280x820."), QStringLiteral("WxH"));
    const QCommandLineOption format(
        QStringLiteral("format"),
        QStringLiteral("The format of the raw captures: s16, r16, s8, r8 or lds (from their extensions when not given). With --rate, they are opened without asking."),
        QStringLiteral("format"));
    const QCommandLineOption rate(QStringLiteral("rate"), QStringLiteral("The sample rate of the raw captures, in MHz. Without it the format, rate and bits are asked for."),
                                  QStringLiteral("MHz"));
    const QCommandLineOption bits(QStringLiteral("bits"), QStringLiteral("The bits the raw captures carry, 1 to 16; found from the samples when not given."), QStringLiteral("n"));
    for (const QCommandLineOption *o : {&view, &screenshot, &size, &format, &rate, &bits}) {
        parser.addOption(*o);
    }
    parser.addOption(QCommandLineOption(QStringLiteral("track"), QStringLiteral("The track to select at the start, from 0."), QStringLiteral("n")));
    parser.addOption(QCommandLineOption(QStringLiteral("align"), QStringLiteral("Start with the time base of every track corrected (Align capture): the speed is worked out from the RF first.")));
    parser.addOption(QCommandLineOption(QStringLiteral("rf-speed-all"), QStringLiteral("Start with the speed measured from the RF for every track (they are worked on together).")));
    parser.addOption(QCommandLineOption(QStringLiteral("rf-speed"), QStringLiteral("Start with the speed deviation measured from the RF (the From RF box).")));
    parser.addOption(QCommandLineOption(QStringLiteral("force-dark-theme"), QStringLiteral("Force the dark theme, whatever the system and the setting say.")));
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("The .rfx files and the captures (.s16 .r16 .s8 .r8 .lds .flac .ldf) to open."),
                                 QStringLiteral("[file...]"));
    parser.process(app);
    // View > Theme is applied by the window from the settings; this overrides it for the session.
    app.setProperty("forceDarkTheme", parser.isSet(QStringLiteral("force-dark-theme")));

    const QStringList args = parser.positionalArguments();

    MainWindow window;
    if (parser.isSet(size)) {
        const QStringList parts = parser.value(size).split(QLatin1Char('x'));
        bool okW = false;
        bool okH = false;
        const int w = parts.value(0).toInt(&okW);
        const int h = parts.value(1).toInt(&okH);
        if (!okW || !okH || w < 400 || h < 300) {
            return usage("--size wants WxH, such as 1280x820");
        }
        window.resize(w, h);
    }
    window.show();

    if (!args.isEmpty()) {
        // A capture says nothing of its format, unless it is compressed: the header of that one gives its rate.
        bool anyCapture = false;
        for (const QString &path : args) {
            anyCapture = anyCapture || (!isArchiveFile(path) && !isCompressedCapture(path) && !rfx::declaredCapture(rfx::findSidecars(path)));
        }
        bool opened = false;
        if (parser.isSet(rate) && std::any_of(args.begin(), args.end(), [](const QString &p) { return !isArchiveFile(p); })) {
            // Everything the captures do not say was given: no question.
            bool ok = false;
            const double mhz = parser.value(rate).toDouble(&ok);
            if (!ok || !(mhz > 0.0)) {
                return usage("--rate wants a positive number of MHz, not '" + parser.value(rate).toStdString() + "'");
            }
            int given = 0;
            if (parser.isSet(bits)) {
                given = parser.value(bits).toInt(&ok);
                if (!ok || given < kFewestBits || given > kMostBits) {
                    return usage("--bits is 1 to 16, not '" + parser.value(bits).toStdString() + "'");
                }
            }
            std::vector<Session::Source> sources;
            for (const QString &path : args) {
                Session::Source s;
                s.path = path;
                if (!isArchiveFile(path)) {
                    s.capture = true;
                    s.spec.rateHz = mhz * 1e6;
                    const std::string name = parser.isSet(format) ? parser.value(format).toStdString() : QFileInfo(path).suffix().toLower().toStdString();
                    if (isCompressedCapture(path)) {
                        // Decoded where it is looked at, as 16-bit samples.
                        s.spec.format = rfx::SampleFormat::S16;
                        s.spec.compressed = true;
                    } else if (!rfx::parseFormat(name, s.spec.format)) {
                        return usage("the format of " + path.toStdString() + " is not known: give --format s16, r16, s8, r8 or lds");
                    }
                    if (given > 0) {
                        s.spec.bits = given;
                    }
                    s.spec.name = QFileInfo(path).fileName().toStdString();
                }
                sources.push_back(s);
            }
            opened = window.openSources(sources);
        } else if (anyCapture && parser.isSet(screenshot)) {
            return usage("a capture needs --rate (and --format) to be opened with no one to ask");
        } else {
            opened = window.openFiles(args);
        }
        if (!opened && parser.isSet(screenshot)) {
            return kExitFailed;
        }
    }
    if (parser.isSet(QStringLiteral("track"))) {
        window.selectTrack(parser.value(QStringLiteral("track")).toInt());
    }
    window.setView(parser.value(view));
    window.setRfSpeed(parser.isSet(QStringLiteral("rf-speed")));
    if (parser.isSet(QStringLiteral("align"))) {
        window.setAlignAll();
    }
    if (parser.isSet(QStringLiteral("rf-speed-all"))) {
        window.setRfSpeedAll();
    }
    if (parser.isSet(screenshot)) {
        return window.saveScreenshot(parser.value(screenshot)) ? 0 : kExitFailed;
    }
    return app.exec();
}
