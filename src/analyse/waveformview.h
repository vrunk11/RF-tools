/******************************************************************************
 * waveformview.h
 * rf-analyse - the RF waveform of the tracks over the window, with the bar
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QVector>
#include <QWidget>

class Session;

// Click puts the bar, a drag selects a zone, the wheel scrolls in time, ctrl and
// the wheel zoom, shift and a drag pan.
class WaveformView : public QWidget
{
    Q_OBJECT

public:
    explicit WaveformView(Session *session, QWidget *parent = nullptr);

    // The tracks drawn (those shown, when empty).
    void setTracks(const QVector<int> &tracks);
    // The gain: 1 fills the graph with the whole range of the converter, at the size it had when it was first drawn.
    void setAmplitude(double zoom);
    // The gain back to 1 at the size the graph has now.
    void resetGain();
    // On, the gain goes up with the height of the graph (and down with it) so that the signal fills the same part of it.
    // Off, the gain stays: the graph shows more (or less) of the signal at the same scale, and the window in time grows
    // with its width.
    void setFollowSize(bool on);
    // This graph is the one whose width sets how much time the window shows.
    void setDrivesWindow(bool on);

    QSize sizeHint() const override { return {400, 150}; }

    // The margin on the left of the plot, where the marks of the levels are.
    static constexpr int kPlotLeft = 54;

signals:
    // The amplitude the graph shows has changed with its size (the scale stays); the number it is now.
    void gainChanged(double gain);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    qint64 sampleAt(double x) const;
    double xOf(qint64 sample) const;
    QVector<int> drawn() const;

    Session *m_session;
    QVector<int> m_tracks;
    double m_gain = 1.0;
    double m_refHeight = 0.0;
    bool m_painted = false;
    int m_lastHeight = 0;
    enum class Drag { None, Select, Pan };
    Drag m_drag = Drag::None;
    double m_pressX = 0.0;
    qint64 m_pressSample = 0;
    qint64 m_pressStart = 0;
    bool m_follow = true;
    bool m_drives = false;
    int m_lastWidth = 0;
    QPoint m_hover = QPoint(-1, -1);
};
