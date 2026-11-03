/******************************************************************************
 * main.cpp
 * rfx-gui - Qt application for .rfx archives of LaserDisc RF captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QLabel>
#include <QMainWindow>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("rfx-gui");
    QApplication::setApplicationVersion(
        QStringLiteral(APP_VERSION " (" APP_BRANCH " " APP_COMMIT ")"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Look at and edit .rfx archives of LaserDisc RF captures");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption selfTest(
        "self-test", "Start, build the main window and exit (used by the CI).");
    selfTest.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(selfTest);
    parser.process(app);

    QMainWindow window;
    window.setWindowTitle("RF Tools");
    window.setCentralWidget(new QLabel("RF Tools"));

    if (parser.isSet(selfTest)) {
        return 0;
    }
    window.resize(800, 600);
    window.show();
    return app.exec();
}
