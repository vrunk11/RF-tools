/******************************************************************************
 * capturedialog.h
 * rf-analyse - what a raw capture does not say of itself: its format, its rate and
 * the bits its samples carry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "trackreader.h"

#include <QDialog>
#include <QStringList>

class QLabel;
class QTableWidget;

// The formats the file chooser offers, as one filter for the files it can open, and
// one for each kind.
QStringList captureFilters();

// Whether `path` is a compressed capture (.flac, .ldf), which ffmpeg decodes first.
bool isCompressedCapture(const QString &path);

// Whether `path` is an .rfx archive, which says what it holds.
bool isArchiveFile(const QString &path);

// Whether `path` is a file the program can open by its extension: an .rfx archive, a raw
// capture (.s16 .r16 .s8 .r8 .lds) or a compressed one (.flac .ldf). What is dropped on the
// window is kept to those.
bool isRfFile(const QString &path);

// The files in a folder that can be opened (see isRfFile), by name; the folders in it are not searched.
QStringList rfFilesInFolder(const QString &folder);

// Asked when files that are not .rfx archives are opened: a row for each, with its
// format (from the extension), its rate (from the .yaml that sits beside the capture
// when there is one, else what was used last) and the bits its samples carry.
class CaptureDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CaptureDialog(const QStringList &paths, QWidget *parent = nullptr);

    int rowCount() const;
    // What was chosen for the file of a row. For a compressed capture the format is
    // the 16-bit one it is decoded to.
    rfx::CaptureSpec spec(int row) const;
    bool compressed(int row) const;

private:
    void formatChanged(int row);
    void remember();

    QStringList m_paths;
    QTableWidget *m_table = nullptr;
    QLabel *m_note = nullptr;
};
