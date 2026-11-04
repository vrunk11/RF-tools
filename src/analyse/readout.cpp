/******************************************************************************
 * readout.cpp
 * rf-analyse - the values under the mouse, drawn on a graph
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "readout.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QPalette>
#include <QPainter>

#include <algorithm>
#include <cmath>

void drawReadout(QPainter &p, const QRect &bounds, const QPoint &at, const QVector<ReadoutLine> &lines)
{
    if (lines.isEmpty()) {
        return;
    }
    const QFontMetrics fm(p.font());
    constexpr int pad = 5;
    constexpr int swatch = 8;
    int width = 0;
    bool anySwatch = false;
    for (const ReadoutLine &l : lines) {
        width = std::max(width, fm.horizontalAdvance(l.text));
        anySwatch = anySwatch || l.color.isValid();
    }
    const int lineHeight = fm.height() + 1;
    const int boxW = width + 2 * pad + (anySwatch ? swatch + 4 : 0);
    const int boxH = static_cast<int>(lines.size()) * lineHeight + 2 * pad - 1;
    // To the right and below the mouse; the other side when that does not fit.
    int x = at.x() + 14;
    int y = at.y() + 14;
    if (x + boxW > bounds.right()) {
        x = at.x() - 14 - boxW;
    }
    if (y + boxH > bounds.bottom()) {
        y = at.y() - 14 - boxH;
    }
    x = std::clamp(x, bounds.left(), std::max(bounds.left(), bounds.right() - boxW));
    y = std::clamp(y, bounds.top(), std::max(bounds.top(), bounds.bottom() - boxH));

    const QPalette pal = QGuiApplication::palette();
    QColor fill = pal.color(QPalette::ToolTipBase);
    fill.setAlpha(235);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(QRect(x, y, boxW, boxH), fill);
    p.setPen(pal.color(QPalette::Mid));
    p.drawRect(QRect(x, y, boxW - 1, boxH - 1));
    int ty = y + pad;
    for (const ReadoutLine &l : lines) {
        int tx = x + pad;
        if (l.color.isValid()) {
            p.fillRect(QRect(tx, ty + (lineHeight - swatch) / 2, swatch, swatch), l.color);
            tx += swatch + 4;
        }
        p.setPen(pal.color(QPalette::ToolTipText));
        p.drawText(QRect(tx, ty, width + 2, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, l.text);
        ty += lineHeight;
    }
    p.restore();
}

void drawAxisLabel(QPainter &p, const QPoint &topLeft, const QString &text)
{
    const QFontMetrics fm(p.font());
    const QPalette pal = QGuiApplication::palette();
    const QRect box(topLeft.x(), topLeft.y(), fm.horizontalAdvance(text) + 6, fm.height() + 2);
    QColor back = pal.color(QPalette::Base);
    back.setAlpha(205);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(box, back);
    QColor ink = pal.color(QPalette::Text);
    ink.setAlpha(230);
    p.setPen(ink);
    p.drawText(box, Qt::AlignCenter, text);
    p.restore();
}

double niceStep(double range, int ticks)
{
    if (!(range > 0.0) || ticks < 1) {
        return 1.0;
    }
    const double rough = range / ticks;
    const double base = std::pow(10.0, std::floor(std::log10(rough)));
    for (const double m : {1.0, 2.0, 5.0, 10.0}) {
        if (rough <= m * base) {
            return m * base;
        }
    }
    return 10.0 * base;
}

void drawGutterLabel(QPainter &p, const QRect &box, int alignment, const QString &text)
{
    const QPalette pal = QGuiApplication::palette();
    QColor ink = pal.color(QPalette::Text);
    ink.setAlpha(215);
    p.save();
    p.setPen(ink);
    p.drawText(box, alignment, text);
    p.restore();
}
