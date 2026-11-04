/******************************************************************************
 * statspanel.cpp
 * rf-analyse - the statistics of the tracks at the bar, or over the zone
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "statspanel.h"

#include "collapse.h"
#include "format.h"
#include "session.h"

#include <QFont>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kColumns = 5;
constexpr int kRowHeight = 22;
constexpr int kMinRows = 6;

} // namespace

StatsPanel::StatsPanel(Session *session, QWidget *parent) : QWidget(parent), m_session(session)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *head = new QWidget(this);
    auto *row = new QHBoxLayout(head);
    row->setContentsMargins(2, 0, 2, 0);
    auto *title = new QLabel(tr("Statistics"), head);
    title->setForegroundRole(QPalette::PlaceholderText);
    m_where = new QLabel(head);
    m_where->setForegroundRole(QPalette::PlaceholderText);
    m_where->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    row->addWidget(title);
    row->addWidget(makeCollapseButton(head, [this](bool reduced) {
        emit reductionChanging(reduced, m_tableFrame->isVisible() ? m_tableFrame->height() : 0);
        m_tableFrame->setVisible(!reduced);
        m_reduced = reduced;
        m_separator->setVisible(reduced);
        fitHeight();
    }));
    row->addStretch();
    row->addWidget(m_where);
    layout->addWidget(head);
    m_separator = makeSeparator(this);
    layout->addWidget(m_separator);

    m_table = new QTableWidget(0, kColumns, this);
    m_table->setHorizontalHeaderLabels({tr("Track"), tr("Mean amplitude"), tr("SNR"), tr("Noise floor"), tr("Effective bits")});
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(kRowHeight);
    // The name of the track in full; the numbers share what is left.
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->setTextElideMode(Qt::ElideNone);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setAlternatingRowColors(false);
    m_table->setShowGrid(false);
    m_table->setFrameShape(QFrame::NoFrame);
    m_tableFrame = sunkenFrame(m_table, this);
    layout->addWidget(m_tableFrame);

    connect(session, &Session::changed, this, &StatsPanel::refresh);
    connect(session, &Session::resultReady, this, &StatsPanel::refresh);
}

void StatsPanel::setTracks(const QVector<int> &tracks)
{
    m_tracks = tracks;
    refresh();
}

void StatsPanel::refresh()
{
    m_table->setRowCount(0);
    m_where->clear();
    if (!m_session->isOpen()) {
        return;
    }
    const AnalysisResult &r = m_session->result();
    const Selection sel = m_session->selection();
    m_where->setText(sel.range ? tr("zone %1 to %2 · %3 samples")
                                     .arg(fmt::milliseconds(m_session->seconds(sel.first)), fmt::milliseconds(m_session->seconds(sel.last)), fmt::count(sel.length))
                               : tr("bar %1 · %2 samples").arg(fmt::milliseconds(m_session->seconds(sel.center)), fmt::count(sel.length)));

    QVector<int> tracks = m_tracks;
    if (tracks.isEmpty()) {
        for (int k = 0; k < m_session->trackCount(); ++k) {
            if (m_session->visible(k)) {
                tracks.push_back(k);
            }
        }
    }
    m_table->setRowCount(tracks.size());
    for (int row = 0; row < tracks.size(); ++row) {
        const int k = tracks[row];
        const bool have = k < static_cast<int>(r.stats.size()) && !r.spectra[static_cast<size_t>(k)].power.empty();
        // The stretch is past the end of a shorter track: say so, rather than wait.
        const bool past = k < static_cast<int>(r.pastEnd.size()) && r.pastEnd[static_cast<size_t>(k)];
        const rfx::SampleStats s = have ? r.stats[static_cast<size_t>(k)] : rfx::SampleStats();
        const auto dash = [&](const QString &text) { return have ? text : (past ? QStringLiteral("-") : QStringLiteral("…")); };
        const QStringList cells = {QString::number(k) + QLatin1Char(' ') + QString::fromStdString(m_session->track(k).name),
                                   past ? tr("past its end") : dash(QString::number(s.meanAmplitude, 'f', 1) + QStringLiteral(" LSB")),
                                   dash(QString::number(s.snrDb, 'f', 1) + QStringLiteral(" dB")),
                                   dash(QString::number(s.noiseFloor, 'f', 2) + QStringLiteral(" LSB")),
                                   dash(QString::number(s.effectiveBits, 'f', 1))};
        for (int c = 0; c < kColumns; ++c) {
            auto *item = new QTableWidgetItem(cells[c]);
            item->setTextAlignment(c == 0 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter);
            if (c == 0) {
                item->setForeground(m_session->color(k));
            }
            m_table->setItem(row, c, item);
        }
    }
    // The rows are as tall as the style makes them (a little more than asked, with some).
    const int rowHeight = m_table->rowCount() > 0 ? std::max(kRowHeight, m_table->rowHeight(0)) : kRowHeight;
    const int header = std::max(m_table->horizontalHeader()->height(), m_table->horizontalHeader()->sizeHint().height());
    const int height = header + rowHeight * std::max<int>(1, tracks.size()) + 4;
    // At least six rows are shown (it scrolls), and all of them when there is room.
    m_table->setMinimumHeight(std::min(height, header + rowHeight * kMinRows + 4));
    fitHeight();
}

// The panel is as tall as what it holds, and no taller: the room that is left is under it, not in it.
void StatsPanel::fitHeight()
{
    const int head = layout()->itemAt(0)->widget()->sizeHint().height();
    const int line = m_separator->height();
    const int spacing = layout()->spacing();
    // Reduced, it is its title bar; otherwise it grows with the room that is free (the table has its rows at least).
    setMaximumHeight(m_reduced ? head + spacing + line + 2 : QWIDGETSIZE_MAX);
}
