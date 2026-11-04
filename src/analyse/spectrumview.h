/******************************************************************************
 * spectrumview.h
 * rf-analyse - the spectrum of the tracks at the bar or over the zone
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QPoint>
#include <QVector>
#include <QWidget>

class Session;

// The spectrum of the tracks. The wheel zooms in frequency around the mouse and a drag moves the view (in frequency;
// and in level, when the view is not normalised).
class SpectrumView : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrumView(Session *session, QWidget *parent = nullptr);

    // The tracks drawn (those shown, when empty).
    void setTracks(const QVector<int> &tracks);
    // Show only the band with the LaserDisc frequencies (the one the statistics are made over).
    void setShowBand(bool on);
    // Normalised: the gain is worked out so that the highest peak is at 90% of the height and the curve fills the
    // graph, whatever its size (the gain says what it comes to). Not: the gain is the one that was set, and a taller
    // graph covers more decibels.
    void setNormalised(bool on);
    // The gain of the level axis (1: 110 dB over the height the graph had when it was first drawn or reset); used when
    // the view is not normalised.
    void setGain(double gain);
    // Back to the whole frequency range, the top of the axis at 0 dB, and the gain 1.
    void resetView();
    // Zoom in frequency (a factor under 1 zooms in), around a point of the view (0 to 1 from the left).
    void zoomFrequency(double factor, double around);

    QSize sizeHint() const override { return {400, 140}; }

signals:
    // The gain the normalisation came to, when it changes (the size of the graph, the signal, the zoom).
    void gainChanged(double gain);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    double fullSpanHz() const;

    Session *m_session;
    QVector<int> m_tracks;
    bool m_showBand = true;
    bool m_normalised = true;
    double m_gain = 1.0;
    // The level at the top of the axis when it is not normalised (what the normalisation left, when it is turned off).
    double m_topDb = 0.0;
    // The part of the frequencies in view, from 0 to 1.
    double m_low = 0.0;
    double m_high = 1.0;
    double m_lastGain = 0.0;
    QPoint m_hover = QPoint(-1, -1);
    bool m_dragging = false;
    QPoint m_dragFrom;
    double m_dragLow = 0.0;
    double m_dragTop = 0.0;
    double m_spanDb = 110.0;
    double m_refHeight = 0.0;
};
