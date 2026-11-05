/******************************************************************************
 * deviationview.h
 * rf-analyse - the speed deviation over time: the lag of each track against its
 * reference, zero in the middle
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QCheckBox>
#include <QVector>
#include <QFrame>
#include <QToolButton>
#include <QWidget>

class Session;

// The curve of every track that has a lag table, over the whole capture, with the
// window the waveform shows and the bar. A click puts the bar there.
class DeviationView : public QWidget
{
    Q_OBJECT

public:
    explicit DeviationView(Session *session, QWidget *parent = nullptr);

    // The tracks drawn (those shown, when empty).
    void setTracks(const QVector<int> &tracks);

    // The tracks the curve is made for: those drawn that are not the primary.
    QVector<int> curves() const;

    QSize sizeHint() const override { return {400, 110}; }

    // The amplitude of the graph: the ppm at its top and at its bottom, or 0 for the largest deviation shown, with a little room.
    void setRange(double ppm);
    double range() const { return m_range; }

signals:
    // The wheel asks for another range (ppm).
    void rangeChosen(double ppm);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    double plotLeft() const;
    double plotRight() const;
    qint64 sampleAt(double x) const;
    double xOf(qint64 sample) const;

    Session *m_session;
    QVector<int> m_tracks;
    double m_range = 0.0;
    double m_shownRange = 100.0;
    bool m_dragging = false;
    QPoint m_hover = QPoint(-1, -1);
};

class QLabel;
class QStackedLayout;
class QComboBox;

// The view with its heading: the title, what the curve is, and the deviation of
// every track at the bar; or, when no track drawn has a lag table (the primary
// track is the reference), the saying so.
class DeviationBlock : public QWidget
{
    Q_OBJECT

public:
    explicit DeviationBlock(Session *session, QWidget *parent = nullptr);

    void setExpandedMinimum(int height);

    // The tracks drawn (those shown, when empty).
    void setTracks(const QVector<int> &tracks);

signals:
    // The block is about to be reduced to its title (or shown again); the height of its graph now.
    void reductionChanging(bool reduced, int bodyHeight);

private:
    void refresh();
    QVector<int> rfTracks() const;
    bool m_rfMode = false;

    Session *m_session;
    QVector<int> m_tracks;
    QLabel *m_values;
    QLabel *m_subtitle;
    QCheckBox *m_rfBox = nullptr;
    QComboBox *m_rangeBox = nullptr;
    QLabel *m_unavailable;
    DeviationView *m_view;
    QWidget *m_head;
    QToolButton *m_collapseButton = nullptr;
    QFrame *m_viewFrame = nullptr;
    QFrame *m_separator = nullptr;
    bool m_collapsed = false;
    int m_expandedMinimum = 0;
};
