/******************************************************************************
 * statspanel.h
 * rf-analyse - the statistics of the tracks at the bar, or over the zone
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QVector>
#include <QFrame>
#include <QWidget>

class QLabel;
class QTableWidget;
class Session;

// Mean amplitude, SNR, noise floor and effective bits of every track shown, made
// over the stretch the bar or the zone stands for.
class StatsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit StatsPanel(Session *session, QWidget *parent = nullptr);

    // The tracks listed (those shown, when empty).
    void setTracks(const QVector<int> &tracks);

signals:
    // The panel is about to be reduced to its title (or shown again); the height of its table now.
    void reductionChanging(bool reduced, int bodyHeight);

private:
    void refresh();

    Session *m_session;
    QVector<int> m_tracks;
    QLabel *m_where;
    QTableWidget *m_table;
    QFrame *m_tableFrame = nullptr;
    QFrame *m_separator = nullptr;
    bool m_reduced = false;
    void fitHeight();
};
