/******************************************************************************
 * processdialog.cpp
 * rf-packer - what to ask before writing, and the window that shows the work going on
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "processdialog.h"

#include "opencl.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryFile>
#include <QThread>
#include <QVBoxLayout>

ProcessDialog::ProcessDialog(const QString &archive, Codec codec, QWidget *parent) : QDialog(parent), m_archive(archive), m_codec(codec)
{
    setWindowTitle(archive.isEmpty() ? tr("Create RFX") : tr("Process"));
    QSettings settings;
    auto *layout = new QVBoxLayout(this);

    if (!archive.isEmpty() && codec == Codec::Rfx) {
        auto *where = new QLabel(tr("The list is written as:"), this);
        m_copy = new QRadioButton(tr("A copy (the opened file stays as it is)"), this);
        m_overwrite = new QRadioButton(tr("Overwrite %1").arg(QFileInfo(archive).fileName()), this);
        m_overwrite->setObjectName(QStringLiteral("overwrite"));
        m_copy->setChecked(true);
        layout->addWidget(where);
        layout->addWidget(m_copy);
        layout->addWidget(m_overwrite);
    }
    auto *pathRow = new QHBoxLayout;
    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("outputPath"));
    auto *browse = new QPushButton(tr("Browse…"), this);
    pathRow->addWidget(new QLabel(tr("File"), this));
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);
    layout->addLayout(pathRow);
    connect(browse, &QPushButton::clicked, this, &ProcessDialog::chooseOutput);
    if (m_overwrite) {
        connect(m_overwrite, &QRadioButton::toggled, this, &ProcessDialog::updateOutput);
    }

    auto *form = new QFormLayout;
    m_level = new QComboBox(this);
    m_level->setObjectName(QStringLiteral("defaultLevel"));
    m_level->addItem(tr("1 - fastest"), 1);
    m_level->addItem(tr("2"), 2);
    m_level->addItem(tr("3 - default"), 3);
    m_level->addItem(tr("4 - strongest"), 4);
    m_level->setCurrentIndex(m_level->findData(settings.value(QStringLiteral("packLevel"), 3).toInt()));
    m_device = new QComboBox(this);
    m_device->addItem(tr("Processor"), QStringLiteral("cpu"));
    m_device->addItem(tr("Graphics card"), QStringLiteral("gpu"));
    // The graphics card unless the person chose otherwise before, when there is one to use.
    bool gpuAvailable = false;
    try {
        gpuAvailable = rfx::cl::Device::get() != nullptr;
    } catch (const std::exception &) {
    }
    m_device->setCurrentIndex(m_device->findData(settings.value(QStringLiteral("packDevice"), gpuAvailable ? QStringLiteral("gpu") : QStringLiteral("cpu")).toString()));
    m_threads = new QSpinBox(this);
    m_threads->setRange(0, 256);
    m_threads->setSpecialValueText(tr("one per processor"));
    m_threads->setValue(settings.value(QStringLiteral("packThreads"), 0).toInt());
    m_verify = new QCheckBox(tr("Decode every segment again and compare it with its samples"), this);
    m_verify->setChecked(settings.value(QStringLiteral("packVerify"), true).toBool());
    m_pass0 = new QCheckBox(tr("Compute TBC curve (the speed of the disc and the time base correction)"), this);
    m_pass0->setChecked(settings.value(QStringLiteral("packPass0"), false).toBool());
    m_pass0->setToolTip(tr("Work out the TBC curve of every track from its RF once the file is written and keep it in the file, so that rf-analyse shows the speed and aligns the captures at once. It takes minutes on large captures."));
    m_rate = new QDoubleSpinBox(this);
    m_rate->setRange(0.001, 1000.0);
    m_rate->setDecimals(4);
    m_rate->setSuffix(QStringLiteral(" MHz"));
    m_rate->setValue(settings.value(QStringLiteral("packRate"), 75.0).toDouble());
    m_rate->setToolTip(tr("The rate of a capture that has no .yaml and no rate of its own; it is used to work out the TBC curve of the captures."));
    if (codec == Codec::Rfx) {
        form->addRow(tr("Level"), m_level);
    } else {
        m_level->hide();
    }
    form->addRow(tr("Device"), m_device);
    form->addRow(tr("Threads"), m_threads);
    if (codec == Codec::Rfx) {
        form->addRow(QString(), m_verify);
        form->addRow(QString(), m_pass0);
    } else {
        m_verify->hide();
        m_pass0->hide();
    }
    form->addRow(tr("Default rate"), m_rate);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(archive.isEmpty() ? tr("Create") : tr("Process"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ProcessDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    if (archive.isEmpty() || codec != Codec::Rfx) {
        {
            const QString base = settings.value(QStringLiteral("lastDir")).toString();
            const QString stem = archive.isEmpty() ? QStringLiteral("new") : QFileInfo(archive).completeBaseName() + (codec == Codec::Flac ? QStringLiteral("-flac") : QStringLiteral("-raw"));
            m_path->setText(QDir::cleanPath(base + QLatin1Char('/') + stem + QStringLiteral(".rfx")));
        }
    } else {
        updateOutput();
    }
}

void ProcessDialog::updateOutput()
{
    if (m_archive.isEmpty()) {
        return;
    }
    const QFileInfo info(m_archive);
    const bool over = overwrites();
    m_path->setEnabled(!over);
    m_path->setText(over ? m_archive : info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral("-edited.rfx"));
}

bool ProcessDialog::overwrites() const
{
    return m_overwrite && m_overwrite->isChecked();
}

QString ProcessDialog::output() const
{
    return m_path->text().trimmed();
}

void ProcessDialog::chooseOutput()
{
    const QString file = QFileDialog::getSaveFileName(this, tr("Write the .rfx file"), m_path->text(), tr("RFX archives (*.rfx)"));
    if (!file.isEmpty()) {
        m_path->setText(file.endsWith(QStringLiteral(".rfx"), Qt::CaseInsensitive) ? file : file + QStringLiteral(".rfx"));
        if (m_copy) {
            m_copy->setChecked(true);
        }
    }
}

rfx::PackOptions ProcessDialog::options() const
{
    rfx::PackOptions o;
    o.order = rfx::OrderMode::Given;
    o.level = m_level->currentData().toInt();
    o.verify = m_verify->isChecked();
    o.keepPass0 = m_pass0->isVisibleTo(const_cast<ProcessDialog *>(this)) && m_pass0->isChecked();
    o.device = m_device->currentData().toString() == QLatin1String("gpu") ? rfx::Device::Gpu : rfx::Device::Cpu;
    o.threads = static_cast<unsigned>(m_threads->value());
    o.rateMhz = m_rate->value();
    return o;
}

void ProcessDialog::accept()
{
    if (output().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Choose the file to write."));
        return;
    }
    if (!overwrites() && QFileInfo::exists(output())) {
        const auto answer = QMessageBox::question(this, windowTitle(), tr("%1 exists. Replace it?").arg(QFileInfo(output()).fileName()));
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    QSettings settings;
    settings.setValue(QStringLiteral("packLevel"), m_level->currentData().toInt());
    settings.setValue(QStringLiteral("packDevice"), m_device->currentData().toString());
    settings.setValue(QStringLiteral("packThreads"), m_threads->value());
    settings.setValue(QStringLiteral("packVerify"), m_verify->isChecked());
    settings.setValue(QStringLiteral("packPass0"), m_pass0->isChecked());
    settings.setValue(QStringLiteral("packRate"), m_rate->value());
    settings.setValue(QStringLiteral("lastDir"), QFileInfo(output()).absolutePath());
    QDialog::accept();
}

namespace {
QString g_worker;
}

void ProgressDialog::setWorkerProgram(const QString &path)
{
    g_worker = path;
}

ProgressDialog::ProgressDialog(const Job &job, QWidget *parent) : QDialog(parent), m_job(job)
{
    setWindowTitle(tr("Writing %1").arg(QFileInfo(job.output).fileName()));
    setMinimumWidth(560);
    auto *layout = new QVBoxLayout(this);
    m_status = new QLabel(tr("Starting…"), this);
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 0);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(180);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    // (nothing to show until the work says something: just the bar, as long as it has nothing else to say)
    m_log->hide();
    m_cancel = new QPushButton(tr("Cancel"), this);
    auto *row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(m_cancel);
    layout->addWidget(m_status);
    layout->addWidget(m_bar);
    layout->addWidget(m_log, 1);
    layout->addLayout(row);
    connect(m_cancel, &QPushButton::clicked, this, &ProgressDialog::cancel);
}

ProgressDialog::~ProgressDialog()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(5000);
        removeJobLeftovers(m_job.output);
    }
    if (!m_jobFile.isEmpty()) {
        QFile::remove(m_jobFile);
    }
}

void ProgressDialog::start()
{
    QTemporaryFile file(QDir::tempPath() + QStringLiteral("/rf-packer-XXXXXX.json"));
    file.setAutoRemove(false);
    if (!file.open()) {
        m_error = tr("The job file cannot be written: %1").arg(file.errorString());
        QMetaObject::invokeMethod(this, &QDialog::reject, Qt::QueuedConnection);
        return;
    }
    file.write(jobToJson(m_job));
    m_jobFile = file.fileName();
    file.close();

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardError, this, &ProgressDialog::readOutput);
    connect(m_process, &QProcess::finished, this, &ProgressDialog::finished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_error = tr("The program could not be started: %1").arg(m_process->errorString());
            reject();
        }
    });
    m_process->start(g_worker.isEmpty() ? QCoreApplication::applicationFilePath() : g_worker, {QStringLiteral("--job"), m_jobFile});
    m_status->setText(m_job.verify ? tr("Checking…") : tr("Writing…"));
}

// What the work says goes in a log that is shown when it has something in it, and the window grows to take it.
void ProgressDialog::appendLog(const QString &line)
{
    m_log->appendPlainText(line);
    if (m_log->isHidden()) {
        m_log->show();
        QMetaObject::invokeMethod(this, [this] { resize(width(), std::max(height(), sizeHint().height())); }, Qt::QueuedConnection);
    }
}

void ProgressDialog::readOutput()
{
    m_pending += QString::fromLocal8Bit(m_process->readAllStandardError());
    // A line is complete at its newline; the library also ends progress lines with a carriage return.
    for (;;) {
        const int cut = m_pending.indexOf(QRegularExpression(QStringLiteral("[\\r\\n]")));
        if (cut < 0) {
            break;
        }
        const QString line = m_pending.left(cut);
        m_pending.remove(0, cut + 1);
        if (line.trimmed().isEmpty()) {
            continue;
        }
        static const QRegularExpression pass0(QStringLiteral("^pass 0 \\[(\\d+)/(\\d+)\\] (.+): (\\d+)%$"));
        static const QRegularExpression coding(QStringLiteral("^coding (.+): (\\d+)%$"));
        static const QRegularExpression verifying(QStringLiteral("^verify: (\\d+)%$"));
        // (the quick look at the system of the lines is made by the window before the work, on its own; the work tells it too, for the command line)
        if (line.startsWith(QLatin1String("systems:")) || line.contains(QLatin1String("the standard said is"))) {
            continue;
        }
        const QRegularExpressionMatch vf = verifying.match(line);
        if (vf.hasMatch()) {
            m_bar->setRange(0, 100);
            m_bar->setValue(vf.captured(1).toInt());
            m_status->setText(tr("Checking the samples against their checksums: %1%").arg(vf.captured(1).toInt()));
            continue;
        }
        const QRegularExpressionMatch p0 = pass0.match(line);
        if (p0.hasMatch()) {
            // Where the lines of the captures are located: the bar goes over all of them.
            const int at = p0.captured(1).toInt();
            const int of = p0.captured(2).toInt();
            const int percent = p0.captured(4).toInt();
            m_bar->setRange(0, of * 100);
            m_bar->setValue((at - 1) * 100 + percent);
            m_status->setText(tr("Working out the TBC curve of %1 (%2 of %3): %4%").arg(p0.captured(3)).arg(at).arg(of).arg(percent));
            if (percent == 100) {
                appendLog(line);
            }
            continue;
        }
        const QRegularExpressionMatch cd = coding.match(line);
        if (cd.hasMatch()) {
            if (m_total > 0) {
                m_bar->setRange(0, m_total * 100);
                m_bar->setValue(m_done * 100 + cd.captured(2).toInt());
            }
            m_status->setText(tr("Coding %1: %2%").arg(cd.captured(1)).arg(cd.captured(2).toInt()));
            continue;
        }
        if (line.startsWith(QLatin1String("hashing and scanning"))) {
            m_status->setText(tr("Reading the captures…"));
        }
        appendLog(line);
        m_tail = line;
        static const QRegularExpression count(QStringLiteral("^(\\d+) track\\(s\\) to write"));
        static const QRegularExpression done(QStringLiteral(": \\d+ -> \\d+ bytes"));
        const QRegularExpressionMatch m = count.match(line);
        if (m.hasMatch()) {
            m_total = m.captured(1).toInt();
            m_bar->setRange(0, m_total * 100);
            m_bar->setValue(0);
        } else if (done.match(line).hasMatch() && m_total > 0) {
            m_done = qMin(m_done + 1, m_total);
            m_bar->setValue(m_done * 100);
            m_status->setText(tr("Track %1 of %2 written").arg(m_done).arg(m_total));
        }
    }
}

void ProgressDialog::finished(int code, QProcess::ExitStatus status)
{
    readOutput();
    if (m_cancelled) {
        return;
    }
    if (status == QProcess::NormalExit && code == 0) {
        accept();
        return;
    }
    m_error = m_tail.isEmpty() ? tr("The work stopped (exit code %1).").arg(code) : m_tail;
    removeJobLeftovers(m_job.output);
    reject();
}

void ProgressDialog::cancel()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        reject();
        return;
    }
    m_cancelled = true;
    m_cancel->setEnabled(false);
    m_status->setText(tr("Stopping…"));
    m_process->kill();
    m_process->waitForFinished(5000);
    removeJobLeftovers(m_job.output);
    reject();
}
