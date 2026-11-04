/******************************************************************************
 * tracklist.cpp
 * rf-analyse - the tracks of the file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "tracklist.h"

#include "format.h"
#include "session.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

namespace {

constexpr int kRowHeight = 44;
constexpr int kCheckWidth = 30;

} // namespace

class TrackDelegate : public QStyledItemDelegate
{
public:
    explicit TrackDelegate(TrackList *list) : QStyledItemDelegate(list), m_list(list) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {0, kRowHeight}; }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QPalette &pal = option.palette;
        const bool selected = option.state & QStyle::State_Selected;
        // A row as in the source list of ld-stacker-gui: a bar of the colour of the track on its left (wider for the one
        // picked), a faint tint of that colour across it (more for the one picked), and a line under it.
        const QColor trackColor = index.data(Qt::DecorationRole).value<QColor>();
        const bool shown = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        QColor tint = trackColor;
        tint.setAlphaF(shown ? (selected ? 0.40 : 0.20) : 0.07);
        p->fillRect(option.rect, tint);
        p->fillRect(QRect(option.rect.left(), option.rect.top(), selected ? 6 : 4, option.rect.height()), trackColor);
        QColor line = pal.color(QPalette::Text);
        line.setAlpha(90);
        p->setPen(line);
        p->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());

        QStyleOptionViewItem box = option;
        box.rect = QRect(option.rect.left() + 10, option.rect.top() + (option.rect.height() - 16) / 2, 16, 16);
        box.state = (option.state & ~QStyle::State_HasFocus) | (index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? QStyle::State_On : QStyle::State_Off) | QStyle::State_Enabled;
        QApplication::style()->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &box, p);

        p->setRenderHint(QPainter::Antialiasing, true);
        p->setBrush(trackColor);
        p->setPen(Qt::NoPen);
        p->drawEllipse(QRectF(option.rect.left() + kCheckWidth + 6, option.rect.top() + (option.rect.height() - 10) / 2.0, 10, 10));
        p->setRenderHint(QPainter::Antialiasing, false);

        const int textLeft = option.rect.left() + kCheckWidth + 18;
        const int arrow = selected ? 16 : 0;
        const QRect text(textLeft, option.rect.top() + 4, option.rect.right() - textLeft - arrow - 4, option.rect.height() - 8);
        QFont f = option.font;
        p->setFont(f);
        p->setPen(pal.color(QPalette::Text));
        const QFontMetrics fm(f);
        p->drawText(QRect(text.left(), text.top(), text.width(), text.height() / 2), Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, text.width()));
        QFont small = f;
        small.setPointSizeF(f.pointSizeF() * 0.85);
        p->setFont(small);
        p->setPen(pal.color(QPalette::PlaceholderText));
        const QFontMetrics sm(small);
        p->drawText(QRect(text.left(), text.top() + text.height() / 2, text.width(), text.height() / 2), Qt::AlignLeft | Qt::AlignVCenter,
                    sm.elidedText(index.data(Qt::ToolTipRole).toString(), Qt::ElideRight, text.width()));
        if (selected) {
            p->setPen(pal.color(QPalette::Text));
            p->setFont(f);
            p->drawText(QRect(option.rect.right() - 18, option.rect.top(), 14, option.rect.height()), Qt::AlignCenter, m_list->m_panelShown ? QStringLiteral("›") : QStringLiteral("‹"));
        }
    }

private:
    TrackList *m_list;
};

TrackList::TrackList(Session *session, QWidget *parent) : QListWidget(parent), m_session(session)
{
    setItemDelegate(new TrackDelegate(this));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformItemSizes(true);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Sunken);
    connect(session, &Session::opened, this, &TrackList::rebuild);
    connect(session, &Session::closed, this, &TrackList::rebuild);
    connect(session, &Session::changed, this, &TrackList::refreshChecks);
}

void TrackList::setPanelShown(bool shown)
{
    m_panelShown = shown;
    viewport()->update();
}

void TrackList::rebuild()
{
    m_updating = true;
    clear();
    for (int k = 0; k < m_session->trackCount(); ++k) {
        const rfx::SourceMeta &sm = m_session->track(k);
        const double rate = sm.rateHz > 0 ? sm.rateHz : m_session->rateHz();
        auto *item = new QListWidgetItem(QString::number(k) + QLatin1Char(' ') + QString::fromStdString(sm.name), this);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setCheckState(m_session->visible(k) ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::DecorationRole, m_session->color(k));
        // The second line: how long it is and what it takes in the file.
        item->setData(Qt::ToolTipRole, fmt::duration(static_cast<double>(sm.words) / rate) + QStringLiteral(" · ") + fmt::bytes(m_session->storedBytes(k)));
    }
    if (count() > 0) {
        setCurrentRow(m_session->selected());
    }
    m_updating = false;
}

void TrackList::refreshChecks()
{
    if (m_updating || count() != m_session->trackCount()) {
        return;
    }
    m_updating = true;
    for (int k = 0; k < count(); ++k) {
        item(k)->setCheckState(m_session->visible(k) ? Qt::Checked : Qt::Unchecked);
    }
    if (currentRow() != m_session->selected()) {
        setCurrentRow(m_session->selected());
    }
    m_updating = false;
    viewport()->update();
}

void TrackList::mousePressEvent(QMouseEvent *event)
{
    QListWidgetItem *clicked = itemAt(event->pos());
    if (!clicked) {
        return;
    }
    const int row = this->row(clicked);
    if (event->pos().x() < kCheckWidth) {
        // The check box: show or hide the track, nothing else.
        m_session->setVisible(row, clicked->checkState() != Qt::Checked);
        return;
    }
    const bool was = row == m_session->selected();
    emit rowClicked(row, was);
}
