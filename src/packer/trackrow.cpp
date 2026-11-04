/******************************************************************************
 * trackrow.cpp
 * rf-packer - a line of the list of tracks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "trackrow.h"

#include "format.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QString sizeText(uint64_t bytes)
{
    return fmt::bytes(bytes);
}

QString tagText(EntryState s)
{
    switch (s) {
    case EntryState::Kept: return QObject::tr("kept");
    case EntryState::Edited: return QObject::tr("edited");
    case EntryState::Recode: return QObject::tr("recode");
    case EntryState::New: return QObject::tr("new");
    case EntryState::Removed: return QObject::tr("removed");
    }
    return {};
}

QColor tagColor(EntryState s)
{
    switch (s) {
    case EntryState::Kept: return QColor(QStringLiteral("#9b9b9b"));
    case EntryState::Edited: return QColor(QStringLiteral("#378ADD"));
    case EntryState::Recode: return QColor(QStringLiteral("#BA7517"));
    case EntryState::New: return QColor(QStringLiteral("#1D9E75"));
    case EntryState::Removed: return QColor(QStringLiteral("#E24B4A"));
    }
    return {};
}

QToolButton *smallButton(const QString &text, const QString &tip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(false);
    b->setFixedSize(22, 22);
    return b;
}

} // namespace

TrackRow::TrackRow(const Entry &entry, int index, EntryState state, const QColor &color, bool picked, bool first, bool last, QWidget *parent)
    : QWidget(parent), m_index(index), m_color(color), m_picked(picked), m_faded(state == EntryState::Removed)
{
    setObjectName(QStringLiteral("trackRow"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 0, 0, 0);
    outer->setSpacing(0);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 6, 6, 6);
    top->setSpacing(8);
    auto *number = new QLabel(QString::number(index), this);
    number->setMinimumWidth(18);
    number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    number->setForegroundRole(QPalette::PlaceholderText);
    top->addWidget(number);

    auto *name = new QLabel(this);
    name->setTextFormat(Qt::RichText);
    const QString shown = entry.name.toHtmlEscaped();
    QString sizes;
    if (entry.fromArchive) {
        sizes = sizeText(entry.meta.size) + QStringLiteral(" → ") + sizeText(entry.packed);
    } else {
        sizes = sizeText(entry.meta.size) + QStringLiteral(" → ") + tr("to code");
    }
    const QString struck = m_faded ? QStringLiteral("<s>%1</s>").arg(shown) : shown;
    name->setText(QStringLiteral("%1<br><small>%2</small>").arg(struck, sizes.toHtmlEscaped()));
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (m_faded) {
        name->setEnabled(false);
    }
    top->addWidget(name, 1);

    auto *tag = new QLabel(tagText(state), this);
    tag->setObjectName(QStringLiteral("rowTag"));
    const QColor c = tagColor(state);
    tag->setStyleSheet(QStringLiteral("QLabel { color: %1; border: 1px solid %1; padding: 0 5px; }").arg(c.name()));
    top->addWidget(tag);

    auto *up = smallButton(QStringLiteral("▲"), tr("Move up"), this);
    up->setObjectName(QStringLiteral("rowUp"));
    up->setEnabled(!first);
    auto *down = smallButton(QStringLiteral("▼"), tr("Move down"), this);
    down->setObjectName(QStringLiteral("rowDown"));
    down->setEnabled(!last);
    const bool removed = state == EntryState::Removed;
    auto *drop = smallButton(removed ? QStringLiteral("↩") : QStringLiteral("✕"), removed ? tr("Keep the track") : tr("Remove the track"), this);
    drop->setObjectName(QStringLiteral("rowRemove"));
    top->addWidget(up);
    top->addWidget(down);
    top->addWidget(drop);
    outer->addLayout(top);

    connect(up, &QToolButton::clicked, this, [this] { emit moved(m_index, -1); });
    connect(down, &QToolButton::clicked, this, [this] { emit moved(m_index, 1); });
    connect(drop, &QToolButton::clicked, this, [this, removed] { removed ? emit restoreAsked(m_index) : emit removeAsked(m_index); });

}

void TrackRow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QColor tint = m_color;
    tint.setAlphaF(m_faded ? 0.07 : (m_picked ? 0.40 : 0.20));
    p.fillRect(rect(), tint);
    p.fillRect(QRect(0, 0, m_picked ? 7 : 5, height()), m_color);
    QColor line = palette().color(QPalette::Text);
    line.setAlpha(90);
    p.setPen(line);
    p.drawLine(0, height() - 1, width(), height() - 1);
}

void TrackRow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        emit picked(m_index);
    }
    QWidget::mousePressEvent(event);
}
