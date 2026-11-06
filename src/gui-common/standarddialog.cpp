/******************************************************************************
 * standarddialog.cpp
 * What the windows share - the tracks whose standard seems wrong, and what to say instead
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "standarddialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString standardName(const QString &key)
{
    if (key.isEmpty()) {
        return QObject::tr("not said");
    }
    return key.toUpper();
}

} // namespace

StandardDialog::StandardDialog(const QList<StandardDoubt> &doubts, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Standard of the tracks"));
    setObjectName(QStringLiteral("standardDialog"));
    setMinimumWidth(460);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("The lines found in these tracks are not those of the standard that was said for them. Choose what to say of each (the cross leaves a track as it is):"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    for (const StandardDoubt &d : doubts) {
        auto *line = new QWidget(this);
        auto *row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        auto *name = new QLabel(tr("%1 (said %2, lines of a %3)").arg(d.name, standardName(d.said), standardName(d.found)), line);
        name->setWordWrap(true);
        row->addWidget(name, 1);
        auto *choice = new QComboBox(line);
        choice->setObjectName(QStringLiteral("standardChoice"));
        for (const char *s : {"pal", "ntsc", "pal-m", "secam", "ntsc-vhd"}) {
            choice->addItem(standardName(QString::fromLatin1(s)), QString::fromLatin1(s));
        }
        choice->setCurrentIndex(std::max(0, choice->findData(d.found)));
        row->addWidget(choice);
        auto *cross = new QPushButton(QStringLiteral("✕"), line);
        cross->setObjectName(QStringLiteral("standardRemove"));
        cross->setToolTip(tr("Leave this track as it is"));
        cross->setFlat(true);
        cross->setFixedWidth(28);
        row->addWidget(cross);
        layout->addWidget(line);
        m_rows.push_back({d.key, choice, line, false});
        const int index = static_cast<int>(m_rows.size()) - 1;
        connect(cross, &QPushButton::clicked, this, [this, index] {
            m_rows[index].removed = true;
            m_rows[index].widget->hide();
            const bool any = std::any_of(m_rows.begin(), m_rows.end(), [](const Row &r) { return !r.removed; });
            if (!any) {
                reject();
            }
        });
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Update"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Leave as they are"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QList<std::pair<int, QString>> StandardDialog::accepted() const
{
    QList<std::pair<int, QString>> out;
    for (const Row &r : m_rows) {
        if (!r.removed) {
            out.push_back({r.key, r.choice->currentData().toString()});
        }
    }
    return out;
}
