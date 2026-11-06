/******************************************************************************
 * trackeditdialog.cpp
 * rf-analyse - what a person says of a track
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "trackeditdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

TrackEditDialog::TrackEditDialog(const rfx::SourceMeta &meta, const std::string &medium, QWidget *parent) : QDialog(parent), m_meta(meta), m_medium(medium)
{
    setWindowTitle(tr("Edit %1").arg(QString::fromStdString(meta.name)));
    setObjectName(QStringLiteral("trackEditDialog"));
    setMinimumWidth(420);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_name = new QLineEdit(QString::fromStdString(meta.name), this);
    m_name->setObjectName(QStringLiteral("editName"));
    form->addRow(tr("Name"), m_name);

    m_source = new QComboBox(this);
    m_source->setObjectName(QStringLiteral("editSource"));
    m_source->addItem(tr("unknown"), QString());
    for (const char *s : {"ld", "vhs", "vhd", "cvbs"}) {
        m_source->addItem(QString::fromStdString(rfx::mediumName(s)), QString::fromLatin1(s));
    }
    m_source->setCurrentIndex(std::max(0, m_source->findData(QString::fromStdString(medium))));
    m_source->setToolTip(tr("What was captured, for the whole file (every track of it). The TBC curve is worked out for LaserDisc captures (LD)."));
    form->addRow(tr("Source (whole file)"), m_source);

    m_standard = new QComboBox(this);
    m_standard->setObjectName(QStringLiteral("editStandard"));
    m_standard->addItem(tr("not said"), QString());
    for (const auto &s : {std::make_pair("pal", "PAL"), std::make_pair("ntsc", "NTSC"), std::make_pair("pal-m", "PAL-M"), std::make_pair("secam", "SECAM"), std::make_pair("ntsc-vhd", "NTSC-VHD")}) {
        m_standard->addItem(QString::fromLatin1(s.second), QString::fromLatin1(s.first));
    }
    m_standard->setCurrentIndex(std::max(0, m_standard->findData(QString::fromStdString(meta.standard))));
    form->addRow(tr("Standard"), m_standard);

    m_reference = new QLineEdit(QString::fromStdString(meta.reference), this);
    m_reference->setObjectName(QStringLiteral("editReference"));
    m_reference->setPlaceholderText(tr("a catalogue number, a title..."));
    form->addRow(tr("Reference"), m_reference);

    m_copy = new QSpinBox(this);
    m_copy->setObjectName(QStringLiteral("editCopy"));
    m_copy->setRange(0, 999);
    m_copy->setSpecialValueText(tr("not said"));
    m_copy->setValue(static_cast<int>(meta.copy));
    m_copy->setToolTip(tr("The number of the physical copy of the disc (the exemplar) the capture was made from"));
    form->addRow(tr("Disc copy"), m_copy);

    m_capture = new QSpinBox(this);
    m_capture->setObjectName(QStringLiteral("editCapture"));
    m_capture->setRange(0, 999);
    m_capture->setSpecialValueText(tr("not said"));
    m_capture->setValue(static_cast<int>(meta.capture));
    m_capture->setToolTip(tr("The number of the capture: a copy of the disc can be captured several times"));
    form->addRow(tr("Capture no."), m_capture);

    m_face = new QComboBox(this);
    m_face->setObjectName(QStringLiteral("editFace"));
    m_face->addItem(tr("not said"), QString());
    for (const char *f : {"A", "B", "AB", "BA"}) {
        m_face->addItem(QString::fromLatin1(f), QString::fromLatin1(f));
    }
    m_face->setCurrentIndex(std::max(0, m_face->findData(QString::fromStdString(meta.face))));
    form->addRow(tr("Face"), m_face);

    m_cvbs = new QPushButton(tr("CVBS"), this);
    m_cvbs->setObjectName(QStringLiteral("editCvbs"));
    m_yc = new QPushButton(tr("Y/C"), this);
    m_yc->setObjectName(QStringLiteral("editYc"));
    for (QPushButton *b : {m_cvbs, m_yc}) {
        b->setCheckable(true);
        b->setToolTip(tr("The signal that was captured: composite (CVBS) or separate luma and chroma (Y/C). Click again to say nothing."));
    }
    m_cvbs->setChecked(meta.signal == "cvbs");
    m_yc->setChecked(meta.signal == "yc");
    connect(m_cvbs, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            m_yc->setChecked(false);
        }
    });
    connect(m_yc, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            m_cvbs->setChecked(false);
        }
    });
    auto *signalRow = new QHBoxLayout;
    signalRow->addWidget(m_cvbs);
    signalRow->addWidget(m_yc);
    signalRow->addStretch();
    form->addRow(tr("Signal"), signalRow);

    m_notes = new QPlainTextEdit(QString::fromStdString(meta.notes), this);
    m_notes->setObjectName(QStringLiteral("editNotes"));
    m_notes->setPlaceholderText(tr("a note about the capture"));
    m_notes->setMinimumHeight(90);
    form->addRow(tr("Notes"), m_notes);
    layout->addLayout(form);

    auto *hint = new QLabel(tr("The file is written again with what is changed here (its samples are copied as they are, nothing is coded again)."), this);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Save in the file"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

rfx::TrackEdit TrackEditDialog::edit() const
{
    rfx::TrackEdit e;
    if (m_name->text() != QString::fromStdString(m_meta.name) && !m_name->text().isEmpty()) {
        e.name = m_name->text().toStdString();
    }
    if (m_notes->toPlainText() != QString::fromStdString(m_meta.notes)) {
        e.notes = m_notes->toPlainText().toStdString();
    }
    if (m_standard->currentData().toString() != QString::fromStdString(m_meta.standard)) {
        e.info.standard = m_standard->currentData().toString().toStdString();
    }
    if (m_reference->text() != QString::fromStdString(m_meta.reference)) {
        e.info.reference = m_reference->text().toStdString();
    }
    if (m_copy->value() != static_cast<int>(m_meta.copy)) {
        e.info.copy = static_cast<uint32_t>(m_copy->value());
    }
    if (m_capture->value() != static_cast<int>(m_meta.capture)) {
        e.info.capture = static_cast<uint32_t>(m_capture->value());
    }
    if (m_face->currentData().toString() != QString::fromStdString(m_meta.face)) {
        e.info.face = m_face->currentData().toString().toStdString();
    }
    const std::string signal = m_cvbs->isChecked() ? "cvbs" : (m_yc->isChecked() ? "yc" : "");
    if (signal != m_meta.signal) {
        e.info.signal = signal;
    }
    return e;
}

std::optional<std::string> TrackEditDialog::medium() const
{
    const std::string chosen = m_source->currentData().toString().toStdString();
    if (chosen == m_medium) {
        return std::nullopt;
    }
    return chosen;
}

bool TrackEditDialog::changed() const
{
    const rfx::TrackEdit e = edit();
    return e.name || e.notes || e.info.any() || medium();
}
