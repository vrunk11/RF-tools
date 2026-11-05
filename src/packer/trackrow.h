/******************************************************************************
 * trackrow.h
 * rf-packer - a line of the list of tracks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef TRACKROW_H
#define TRACKROW_H

#include "document.h"

#include <QColor>
#include <QWidget>

// A line as in the list of ld-stacker-gui: a bar of the colour of the track on the left (wider when it is the one picked), a
// faint tint of that colour across, the number, the name with what is known of it, how it stands (kept, new, recode, removed),
// and the buttons to move it and to take it out.
class TrackRow : public QWidget
{
    Q_OBJECT

public:
    TrackRow(const Entry &entry, int index, EntryState state, const QColor &color, bool picked, bool first, bool last, QWidget *parent = nullptr);

signals:
    void picked(int index);
    void moved(int index, int delta);
    void removeAsked(int index);
    void unpackAsked(int index);
    void restoreAsked(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    int m_index;
    QColor m_color;
    bool m_picked;
    bool m_faded;
};

#endif // TRACKROW_H
