/******************************************************************************
 * processdialog.h
 * rf-packer - what to ask before writing, and the window that shows the work going on
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef PROCESSDIALOG_H
#define PROCESSDIALOG_H

#include "job.h"

#include <QDialog>
#include <QProcess>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;

// Where to write (a copy, or over the archive that was opened) and how to code what has to be coded.
class ProcessDialog : public QDialog
{
    Q_OBJECT

public:
    ProcessDialog(const QString &archive, Codec codec, QWidget *parent = nullptr);

    QString output() const;
    bool overwrites() const;
    rfx::PackOptions options() const;

    void accept() override;

private:
    void chooseOutput();
    void updateOutput();

    QString m_archive;
    Codec m_codec = Codec::Rfx;
    QRadioButton *m_copy = nullptr;
    QRadioButton *m_overwrite = nullptr;
    QLineEdit *m_path = nullptr;
    QComboBox *m_level = nullptr;
    QComboBox *m_device = nullptr;
    QSpinBox *m_threads = nullptr;
    QCheckBox *m_verify = nullptr;
    QDoubleSpinBox *m_rate = nullptr;
};

// Runs the job in the program itself started again (so that it can be stopped), and shows what it says.
class ProgressDialog : public QDialog
{
    Q_OBJECT

public:
    ProgressDialog(const Job &job, QWidget *parent = nullptr);
    ~ProgressDialog() override;

    // Starts the work; the dialog is shown by exec().
    void start();
    // The program that does the work (the one that runs; the tests give rf-packer).
    static void setWorkerProgram(const QString &path);
    QString error() const { return m_error; }

private:
    void readOutput();
    void finished(int code, QProcess::ExitStatus status);
    void cancel();

    Job m_job;
    QProcess *m_process = nullptr;
    QString m_jobFile;
    QString m_error;
    QString m_pending;
    QString m_tail;
    int m_total = 0;
    int m_done = 0;
    bool m_cancelled = false;
    QLabel *m_status = nullptr;
    QProgressBar *m_bar = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_cancel = nullptr;
};

#endif // PROCESSDIALOG_H
