/******************************************************************************
 * waveformview.cpp
 * rf-analyse - the RF waveform of the tracks over the window, with the bar
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "waveformview.h"

#include "format.h"
#include "readout.h"
#include "session.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

// The margins where the marks of the axes are: on the left the levels, at the bottom the times.
constexpr int kGutterLeft = WaveformView::kPlotLeft;
constexpr int kGutterBottom = 20;
// About this many marks on each axis.
constexpr int kMarks = 6;
constexpr double kDragThreshold = 3.0;
// The wheel scrolls this fraction of the window a notch.
constexpr double kScrollFraction = 1.0 / 8.0;

} // namespace

WaveformView::WaveformView(Session *session, QWidget *parent) : QWidget(parent), m_session(session)
{
    setMinimumHeight(90);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(session, &Session::changed, this, qOverload<>(&QWidget::update));
    connect(session, &Session::resultReady, this, qOverload<>(&QWidget::update));
}

void WaveformView::setTracks(const QVector<int> &tracks)
{
    m_tracks = tracks;
    update();
}

void WaveformView::setFollowSize(bool on)
{
    m_follow = on;
    update();
}

// Back to the default: the gain is 1 for the size the graph has now.
void WaveformView::resetGain()
{
    m_gain = 1.0;
    m_refHeight = 0;
    update();
}

void WaveformView::setDrivesWindow(bool on)
{
    m_drives = on;
    m_lastWidth = width();
}

void WaveformView::resizeEvent(QResizeEvent *)
{
    // Following the size, the gain goes up with the height (down with it) so that the signal fills the same part of the
    // graph; the number says so. Not following, the gain stays and the axis goes further out.
    const int plotHeight = height() - kGutterBottom;
    // (only a graph that is on the screen and has been drawn follows: while a view is made, or put away, its size is not
    // a resize by the user, and the number must not change with it; it takes its reference again where it is shown)
    const bool live = m_painted && isVisible();
    if (!live) {
        m_refHeight = 0;
    }
    if (live && m_follow && m_lastHeight > 0 && plotHeight > 0 && plotHeight != m_lastHeight) {
        m_gain *= static_cast<double>(plotHeight) / m_lastHeight;
        if (m_drives) {
            emit gainChanged(m_gain);
        }
    }
    m_lastHeight = plotHeight;
    // Wider, it shows more time at the same scale: the window of the session follows (once, by the graph that drives it).
    if (live && !m_follow && m_drives && m_lastWidth > 0 && width() > 0 && m_session->isOpen() && width() != m_lastWidth) {
        m_session->zoomBy(static_cast<double>(width()) / m_lastWidth, m_session->selection().center);
    }
    m_lastWidth = width();
}

void WaveformView::leaveEvent(QEvent *)
{
    m_hover = QPoint(-1, -1);
    update();
}

void WaveformView::setAmplitude(double zoom)
{
    m_gain = std::max(0.01, zoom);
    update();
}

QVector<int> WaveformView::drawn() const
{
    if (!m_tracks.isEmpty()) {
        return m_tracks;
    }
    QVector<int> shown;
    for (int k = 0; k < m_session->trackCount(); ++k) {
        if (m_session->visible(k)) {
            shown.push_back(k);
        }
    }
    return shown;
}

qint64 WaveformView::sampleAt(double x) const
{
    return m_session->windowStart() + static_cast<qint64>(std::llround((x - kGutterLeft) / std::max(1, width() - kGutterLeft) * static_cast<double>(m_session->windowLength())));
}

double WaveformView::xOf(qint64 sample) const
{
    return kGutterLeft + static_cast<double>(sample - m_session->windowStart()) / static_cast<double>(m_session->windowLength()) * (width() - kGutterLeft);
}

void WaveformView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QPalette &pal = palette();
    p.fillRect(rect(), pal.base());
    if (!m_session->isOpen()) {
        return;
    }
    const int w = width();
    const int L = kGutterLeft;
    const int pw = w - L;                  // the width of the plot
    const int h = height() - kGutterBottom;  // its height
    if (pw < 10 || h < 10) {
        return;
    }
    // Not following the size, the scale (pixels for a unit of level) stays when the window is resized, and the axis goes
    // further out or comes in; following it, the range stays and the picture is stretched.
    const double fullScale = std::ldexp(1.0, m_session->displayBits() - 1);
    // A gain of 1 is the height the graph had when it was first drawn (or reset) filled by the whole range.
    if (m_refHeight <= 0) {
        m_refHeight = h;
    }
    m_painted = true;
    const double full = fullScale * (h / m_refHeight) / m_gain;
    p.setFont(font());
    const int fh = p.fontMetrics().height();

    // The grid and the marks of the axes, in the margins: levels on the left, times at the bottom.
    QColor grid = pal.color(QPalette::Mid);
    grid.setAlpha(150);
    {
        const double step = niceStep(2.0 * full, kMarks);
        for (double v = std::ceil(-full / step) * step; v <= full + step * 1e-6; v += step) {
            const int y = static_cast<int>(std::lround(h / 2.0 - v / full * h / 2.0));
            p.setPen(QPen(v == 0.0 ? pal.color(QPalette::Text) : grid, 1, v == 0.0 ? Qt::DotLine : Qt::SolidLine));
            p.drawLine(L, y, w, y);
            QString text = QString::number(v, 'g', 4);
            if (v > 0.0) {
                text = QStringLiteral("+") + text;
            }
            drawGutterLabel(p, QRect(0, y - fh / 2, L - 6, fh), Qt::AlignRight | Qt::AlignVCenter, text);
        }
        const double startMs = m_session->seconds(m_session->windowStart()) * 1000.0;
        const double spanMs = m_session->seconds(m_session->windowLength()) * 1000.0;
        const double tstep = niceStep(spanMs, kMarks);
        const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(tstep))), 0, 4);
        for (double ms = std::ceil(startMs / tstep) * tstep; ms <= startMs + spanMs; ms += tstep) {
            const int x = static_cast<int>(std::lround(xOf(static_cast<qint64>(ms / 1000.0 * m_session->rateHz()))));
            if (x < L || x > w) {
                continue;
            }
            p.setPen(QPen(grid, 1));
            p.drawLine(x, 0, x, h);
            drawGutterLabel(p, QRect(x - 40, h + 3, 80, fh), Qt::AlignHCenter | Qt::AlignTop, QString::number(ms, 'f', decimals) + QStringLiteral(" ms"));
        }
        p.setPen(QPen(grid, 1));
        p.drawRect(L, 0, pw - 1, h);
    }
    // What is drawn from here on stays inside the plot.
    p.setClipRect(QRect(L, 0, pw, h + 1));

    const Selection sel = m_session->selection();
    QColor accent = pal.color(QPalette::Highlight);
    QColor tint = accent;
    tint.setAlpha(sel.range ? 70 : 38);
    const double x0 = xOf(sel.first);
    const double x1 = xOf(sel.last);
    p.fillRect(QRectF(x0, 0, std::max(1.0, x1 - x0), h), tint);

    // What the worker made last, where it belongs on the screen now: while a new
    // window is being worked out the old one stays, moved.
    const AnalysisResult &r = m_session->result();
    const double n = static_cast<double>(m_session->windowLength());
    const QVector<int> tracks = drawn();
    const auto yOf = [&](double v) { return std::clamp(h / 2.0 - v / full * h / 2.0, 0.0, static_cast<double>(h)); };
    const double perPixel = n / std::max(1, pw);
    // A curve: its samples from the start of the window, as many as are inside the track; a line, or a band of the width of the value either side of 0.
    const auto drawSeries = [&](const std::vector<float> &y, size_t inside, const QColor &color, double width, bool band) {
        if (perPixel < 2.0 && !band) {
            p.setPen(QPen(color, width));
            QPainterPath path;
            for (size_t i = 0; i < inside; ++i) {
                const QPointF pt(xOf(r.windowStart + static_cast<qint64>(i)), yOf(y[i]));
                i ? path.lineTo(pt) : path.moveTo(pt);
            }
            p.drawPath(path);
            return;
        }
        // More samples than pixels (or a band): the extremes of each column.
        std::vector<float> lows(static_cast<size_t>(pw), 1e30F), highs(static_cast<size_t>(pw), -1e30F);
        for (size_t i = 0; i < inside; ++i) {
            const int x = static_cast<int>(xOf(r.windowStart + static_cast<qint64>(i))) - L;
            if (x < 0 || x >= pw) {
                continue;
            }
            lows[static_cast<size_t>(x)] = std::min(lows[static_cast<size_t>(x)], y[i]);
            highs[static_cast<size_t>(x)] = std::max(highs[static_cast<size_t>(x)], y[i]);
        }
        for (int x = 0; x < pw; ++x) {
            if (highs[static_cast<size_t>(x)] < lows[static_cast<size_t>(x)]) {
                continue;
            }
            if (band) {
                // (the spread: from minus its largest in the column to plus it)
                QColor fill = color;
                fill.setAlpha(110);
                p.setPen(QPen(fill, 1));
                p.drawLine(QPointF(L + x + 0.5, yOf(highs[static_cast<size_t>(x)])), QPointF(L + x + 0.5, yOf(-highs[static_cast<size_t>(x)])));
            } else {
                p.setPen(QPen(color, width));
                p.drawLine(QPointF(L + x + 0.5, yOf(highs[static_cast<size_t>(x)])), QPointF(L + x + 0.5, yOf(lows[static_cast<size_t>(x)])));
            }
        }
    };
    if (isCombined() && !r.traces.empty() && (m_session->stackShown() || m_session->residueShown())) {
        // The stack and the noise it removes, in place of the tracks.
        for (const Trace &trace : r.traces) {
            QColor c = trace.track >= 0 ? m_session->color(trace.track) : pal.color(QPalette::Text);
            if (trace.band) {
                c = pal.color(QPalette::Highlight);
            }
            drawSeries(trace.wave, trace.wave.size(), c, trace.track < 0 && !trace.band ? 1.6 : 1.0, trace.band);
        }
    } else {
        for (int k : tracks) {
            if (k >= static_cast<int>(r.wave.size()) || r.wave[static_cast<size_t>(k)].empty()) {
                continue;
            }
            const std::vector<float> &y = r.wave[static_cast<size_t>(k)];
            // A track shorter than the longest ends where it ends: nothing is drawn after.
            const qint64 words = static_cast<qint64>(m_session->track(k).words);
            const size_t inside = static_cast<size_t>(std::clamp<qint64>(words - r.windowStart, 0, static_cast<qint64>(y.size())));
            QColor c = m_session->color(k);
            const bool primary = k == m_session->selected() && tracks.size() > 1;
            c.setAlpha(primary || tracks.size() == 1 ? 255 : 200);
            drawSeries(y, inside, c, primary ? 1.6 : 1.0, false);
            // Where this track ends, when that is on the screen: a dashed line in its colour.
            if (words > r.windowStart && words < r.windowStart + static_cast<qint64>(y.size())) {
                QPen dashed(m_session->color(k), 1.2, Qt::DashLine);
                p.setPen(dashed);
                const double ex = xOf(words);
                p.drawLine(QPointF(ex, 0), QPointF(ex, h));
                p.drawText(QPointF(std::min(ex + 3, static_cast<double>(w - 60)), 12 + 12 * (k % 4)), tr("end of %1").arg(k));
            }
        }
    }
    // The bar, or the edges of the zone.
    p.setPen(QPen(accent, 2));
    p.setBrush(accent);
    const auto edge = [&](double x) {
        p.drawLine(QPointF(x, 0), QPointF(x, h));
        const QPointF tri[3] = {{x - 5, 0}, {x + 5, 0}, {x, 8}};
        p.drawPolygon(tri, 3);
    };
    if (sel.range) {
        edge(x0);
        edge(x1);
    } else {
        const double x = xOf(sel.center);
        edge(x);
        const QString label = fmt::milliseconds(m_session->seconds(sel.center));
        const int tw = p.fontMetrics().horizontalAdvance(label);
        const QRectF box(std::clamp(x + 6, static_cast<double>(L + 2), static_cast<double>(w - tw - 10)), h - 20, tw + 8, 16);
        p.fillRect(box, pal.base());
        p.setPen(accent);
        p.drawText(box, Qt::AlignCenter, label);
    }

    // The values under the mouse: a hair line, and the time, the level at the mouse and each track's sample there.
    if (m_hover.x() >= L && m_hover.y() <= h && m_drag == Drag::None) {
        QColor hair = pal.color(QPalette::Text);
        hair.setAlpha(110);
        p.setPen(QPen(hair, 1, Qt::DotLine));
        p.drawLine(QPointF(m_hover.x() + 0.5, 0), QPointF(m_hover.x() + 0.5, h));
        const qint64 at = sampleAt(m_hover.x());
        QVector<ReadoutLine> lines;
        lines.push_back({tr("%1 · sample %2").arg(fmt::milliseconds(m_session->seconds(at)), fmt::count(at)), QColor()});
        const bool traceMode = isCombined() && !r.traces.empty() && (m_session->stackShown() || m_session->residueShown());
        if (traceMode) {
            for (const Trace &trace : r.traces) {
                const qint64 i = at - r.windowStart;
                if (i < 0 || i >= static_cast<qint64>(trace.wave.size())) {
                    continue;
                }
                lines.push_back({QStringLiteral("%1: %2").arg(trace.label).arg(QString::number(static_cast<double>(trace.wave[static_cast<size_t>(i)]), 'f', 1)), trace.track >= 0 ? m_session->color(trace.track) : QColor()});
            }
        }
        for (int k : tracks) {
            if (traceMode || k >= static_cast<int>(r.wave.size())) {
                continue;
            }
            const std::vector<float> &y = r.wave[static_cast<size_t>(k)];
            const qint64 i = at - r.windowStart;
            if (i < 0 || i >= static_cast<qint64>(y.size()) || at >= static_cast<qint64>(m_session->track(k).words)) {
                continue;
            }
            const double value = static_cast<double>(y[static_cast<size_t>(i)]);
            lines.push_back({QStringLiteral("%1: %2").arg(k).arg(QString::number(value, 'f', 1)), m_session->color(k)});
        }
        p.setFont(font());
        drawReadout(p, QRect(L, 0, pw, h), m_hover, lines);
    }
}

void WaveformView::mousePressEvent(QMouseEvent *event)
{
    if (!m_session->isOpen() || event->button() != Qt::LeftButton) {
        return;
    }
    m_pressX = event->position().x();
    m_pressSample = sampleAt(m_pressX);
    m_pressStart = m_session->windowStart();
    if (event->modifiers() & Qt::ShiftModifier) {
        m_drag = Drag::Pan;
    } else {
        m_drag = Drag::Select;
        m_session->setCursor(m_pressSample);
    }
}

void WaveformView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag == Drag::None) {
        m_hover = event->position().toPoint();
        update();
        return;
    }
    m_hover = event->position().toPoint();
    const double x = event->position().x();
    if (m_drag == Drag::Pan) {
        const double perPixel = static_cast<double>(m_session->windowLength()) / std::max(1, width() - kGutterLeft);
        m_session->setWindowStart(m_pressStart - static_cast<qint64>((x - m_pressX) * perPixel));
    } else if (std::fabs(x - m_pressX) > kDragThreshold) {
        m_session->setRange(m_pressSample, sampleAt(x));
    }
}

void WaveformView::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = Drag::None;
}

void WaveformView::wheelEvent(QWheelEvent *event)
{
    if (!m_session->isOpen()) {
        return;
    }
    const int steps = event->angleDelta().y();
    if (steps == 0) {
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        m_session->zoomBy(steps > 0 ? 0.5 : 2.0, m_session->selection().center);
    } else {
        const qint64 delta = static_cast<qint64>(static_cast<double>(m_session->windowLength()) * kScrollFraction);
        m_session->scrollWindow(steps > 0 ? -delta : delta);
    }
    event->accept();
}
