/******************************************************************************
 * standarddialog.h
 * What the windows share - the tracks whose standard seems wrong, and what to say instead
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef STANDARDDIALOG_H
#define STANDARDDIALOG_H

#include <QDialog>
#include <QList>
#include <QString>

class QComboBox;
class QWidget;

// One track whose standard does not agree with the lines found in it.
struct StandardDoubt
{
    // What the track is called, and what the window needs to find it again (a number it gave).
    QString name;
    int key = 0;
    // The standard said now (the key of the list below, "pal"...), and the one the lines say ("pal" or "ntsc").
    QString said;
    QString found;
};

// Asked once the TBC curve of some tracks is worked out and the lines are not of the system their standard says: the tracks with a list of
// standards to choose from (the one the lines say is chosen), and a cross to take a track off the list (it is then left as it is).
class StandardDialog : public QDialog
{
    Q_OBJECT

public:
    StandardDialog(const QList<StandardDoubt> &doubts, QWidget *parent = nullptr);

    // The tracks to change, by the key they were given, and the standard to say of them.
    QList<std::pair<int, QString>> accepted() const;

private:
    struct Row
    {
        int key = 0;
        QComboBox *choice = nullptr;
        QWidget *widget = nullptr;
        // The line between this track and the one before it (none for the first).
        QWidget *separator = nullptr;
        bool removed = false;
    };
    QList<Row> m_rows;
};

#endif // STANDARDDIALOG_H
