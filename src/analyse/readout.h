/******************************************************************************
 * readout.h
 * rf-analyse - the values under the mouse, drawn on a graph
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef READOUT_H
#define READOUT_H

#include <QColor>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

class QPainter;

struct ReadoutLine
{
    QString text;
    QColor color;   // of the little square before the text (invalid: none)
};

// A box with the lines, near `at` and inside `bounds` (on the other side of the mouse when there is no room).
void drawReadout(QPainter &p, const QRect &bounds, const QPoint &at, const QVector<ReadoutLine> &lines);

// A label of an axis, with a backing of the colour of the graph so that it can be read over the curves and the grid; its
// top left corner is at `topLeft`.
void drawAxisLabel(QPainter &p, const QPoint &topLeft, const QString &text);

// A step of 1, 2 or 5 times a power of ten that makes about `ticks` marks over `range`.
double niceStep(double range, int ticks);

// The text of a mark of an axis, in the margin of a graph (`alignment` is that of Qt::AlignmentFlag).
void drawGutterLabel(QPainter &p, const QRect &box, int alignment, const QString &text);

#endif // READOUT_H
