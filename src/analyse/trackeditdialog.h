/******************************************************************************
 * trackeditdialog.h
 * rf-analyse - what a person says of a track: its name, a note, and what is known of the disc and of how the capture was made
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef TRACKEDITDIALOG_H
#define TRACKEDITDIALOG_H

#include "edit.h"
#include "metadata.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class TrackEditDialog : public QDialog
{
    Q_OBJECT

public:
    // `meta`: the track as it is in its file; `medium`: what was captured, said of the whole file.
    TrackEditDialog(const rfx::SourceMeta &meta, const std::string &medium, QWidget *parent = nullptr);

    // What the person changed, and only that.
    rfx::TrackEdit edit() const;
    // What was captured, of the whole file, when the person changed it.
    std::optional<std::string> medium() const;
    bool changed() const;

private:
    rfx::SourceMeta m_meta;
    std::string m_medium;
    QComboBox *m_source = nullptr;
    QLineEdit *m_name = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QComboBox *m_standard = nullptr;
    QLineEdit *m_reference = nullptr;
    QSpinBox *m_copy = nullptr;
    QSpinBox *m_capture = nullptr;
    QComboBox *m_face = nullptr;
    QPushButton *m_cvbs = nullptr;
    QPushButton *m_yc = nullptr;
};

#endif // TRACKEDITDIALOG_H
