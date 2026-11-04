/******************************************************************************
 * main.cpp
 * rf-packer - make an .rfx file from captures, or change the tracks of one
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "error.h"
#include "job.h"
#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>

#include <cstdio>
#include <cstring>

namespace {

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

// The work, with no window: what the window runs again to do it (so that it can be stopped, and to show what is said).
int runWorker(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const int at = static_cast<int>(args.indexOf(QStringLiteral("--job")));
    if (at < 0 || at + 1 >= args.size()) {
        std::fprintf(stderr, "rf-packer: --job wants the file that describes the work\n");
        return kExitUsage;
    }
    QFile file(args.at(at + 1));
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "rf-packer: %s cannot be read\n", qPrintable(file.fileName()));
        return kExitFailed;
    }
    try {
        runJob(jobFromJson(file.readAll()));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kExitFailed;
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--job") == 0) {
            return runWorker(argc, argv);
        }
    }

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("rf-packer"));
    QCoreApplication::setOrganizationName(QStringLiteral("RF-tools"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION " (" APP_BRANCH " " APP_COMMIT ")"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Make an .rfx file from captures, or open one to read its metadata and change its tracks: add, remove, reorder, code again, edit the name, rate, files and notes."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption screenshot(QStringLiteral("screenshot"), QStringLiteral("Write what the window shows to a PNG file and quit (for documentation and tests)."),
                                        QStringLiteral("file.png"));
    const QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Size of the window, as 900x640."), QStringLiteral("WxH"));
    parser.addOption(screenshot);
    parser.addOption(size);
    parser.addOption(QCommandLineOption(QStringLiteral("force-dark-theme"), QStringLiteral("Force the dark theme, whatever the system and the setting say.")));
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("An .rfx file to open, and captures (.s16 .r16 .s8 .r8 .lds .flac .ldf) to add to the list."), QStringLiteral("[file...]"));
    parser.process(app);
    app.setProperty("forceDarkTheme", parser.isSet(QStringLiteral("force-dark-theme")));

    MainWindow window;
    if (parser.isSet(size)) {
        const QStringList parts = parser.value(size).split(QLatin1Char('x'));
        bool okW = false;
        bool okH = false;
        const int w = parts.value(0).toInt(&okW);
        const int h = parts.value(1).toInt(&okH);
        if (!okW || !okH || w < 400 || h < 300) {
            std::fprintf(stderr, "rf-packer: --size wants WxH, such as 900x640\n");
            return kExitUsage;
        }
        window.resize(w, h);
    }
    window.show();

    QStringList captures;
    for (const QString &path : parser.positionalArguments()) {
        if (path.endsWith(QStringLiteral(".rfx"), Qt::CaseInsensitive)) {
            if (!window.openArchive(path) && parser.isSet(screenshot)) {
                return kExitFailed;
            }
        } else {
            captures << path;
        }
    }
    if (!captures.isEmpty() && !window.addCaptures(captures) && parser.isSet(screenshot)) {
        return kExitFailed;
    }
    if (parser.isSet(screenshot)) {
        return window.saveScreenshot(parser.value(screenshot)) ? 0 : kExitFailed;
    }
    return app.exec();
}
