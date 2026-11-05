/******************************************************************************
 * spectrumview.cpp
 * rf-analyse - the spectrum of the tracks at the bar or over the zone
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "spectrumview.h"

#include "readout.h"
#include "session.h"

#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <map>
#include <memory>

#include <algorithm>
#include <cmath>

namespace {

// The decibels the axis covers with a gain of 1.
constexpr double kSpanDb = 110.0;
// The margins where the marks of the axes are: on the left the levels, at the bottom the frequencies.
// The curves of the stack and the residue are numbered from here, after the tracks.
constexpr int kTraceBase = 1000;
constexpr int kGutterLeft = 54;
constexpr int kGutterBottom = 20;
// About this many marks on each axis.
constexpr int kMarks = 6;
// Normalised, the highest peak is at this part of the height, and the quiet part of the curve (its tenth percentile)
// at about a fifth.
constexpr double kPeakAt = 0.9;
constexpr double kFloorAt = 0.2;
constexpr double kLeastSpanDb = 20.0;
constexpr double kMostSpanDb = 220.0;
// The least part of the frequencies that can be in view.
constexpr double kLeastView = 0.01;
// What a notch of the wheel zooms.
constexpr double kWheelZoom = 0.8;

} // namespace

SpectrumView::SpectrumView(Session *session, QWidget *parent) : QWidget(parent), m_session(session)
{
    setMinimumHeight(90);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(session, &Session::changed, this, qOverload<>(&QWidget::update));
    connect(session, &Session::resultReady, this, qOverload<>(&QWidget::update));
    // When the result is late, the rough spectrum of the capture is shown meanwhile (for the captures that are decoded as
    // they are looked at); when there is one, a late result is shown with it.
    connect(session, &Session::staleResult, this, qOverload<>(&QWidget::update));
    connect(session, &Session::overviewReady, this, qOverload<>(&QWidget::update));
}

void SpectrumView::setTracks(const QVector<int> &tracks)
{
    m_tracks = tracks;
    update();
}

void SpectrumView::setNormalised(bool on)
{
    m_normalised = on;
    m_lastGain = 0.0;
    update();
}

void SpectrumView::setGain(double gain)
{
    m_gain = std::max(0.05, gain);
    update();
}

void SpectrumView::resetView()
{
    m_low = 0.0;
    m_high = 1.0;
    m_gain = 1.0;
    m_topDb = 0.0;
    m_lastGain = 0.0;
    m_refHeight = 0.0;
    update();
}

void SpectrumView::setShowBand(bool on)
{
    m_showBand = on;
    update();
}

// With the band on, only the part of the spectrum with the LaserDisc frequencies can be drawn (the band the statistics
// are made over); off, it goes up to half the sample rate.
double SpectrumView::fullSpanHz() const
{
    return m_showBand ? m_session->bandHighHz() : m_session->rateHz() / 2.0;
}

void SpectrumView::zoomFrequency(double factor, double around)
{
    const double range = m_high - m_low;
    const double newRange = std::clamp(range * factor, kLeastView, 1.0);
    const double a = std::clamp(around, 0.0, 1.0);
    const double point = m_low + a * range;
    const double low = std::clamp(point - a * newRange, 0.0, 1.0 - newRange);
    m_low = low;
    m_high = low + newRange;
    update();
}

void SpectrumView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QPalette &pal = palette();
    p.fillRect(rect(), pal.base());
    if (!m_session->isOpen()) {
        return;
    }
    const int w = width();
    const int L = kGutterLeft;
    const int pw = w - L;                    // the width of the plot
    const int h = height() - kGutterBottom;  // its height
    if (pw < 10 || h < 10) {
        return;
    }
    const double full = fullSpanHz();
    const double lo = m_low * full;
    const double span = std::max(1.0, (m_high - m_low) * full);

    const AnalysisResult &r = m_session->result();
    // The spectrum of a track: the one of the result, or while that is late the rough one of the capture (when it has one).
    std::map<int, std::shared_ptr<const rfx::Spectrum>> rough;
    const bool late = m_session->coarseWanted();
    bool shownRough = false;
    // With the stack or the residue asked for, the curves of the combined view are those of the result (numbered from kTraceBase).
    const bool traceMode = m_tracks.isEmpty() && !r.traces.empty() && (m_session->stackShown() || m_session->residueShown());
    const auto spectrumOf = [&](int k) -> const rfx::Spectrum * {
        if (k >= kTraceBase) {
            return static_cast<size_t>(k - kTraceBase) < r.traces.size() ? &r.traces[static_cast<size_t>(k - kTraceBase)].spectrum : nullptr;
        }
        if (late) {
            auto it = rough.find(k);
            if (it == rough.end()) {
                it = rough.emplace(k, m_session->coarseSpectrum(k)).first;
            }
            if (it->second) {
                shownRough = true;
                return it->second.get();
            }
        }
        return k >= 0 && k < static_cast<int>(r.spectra.size()) ? &r.spectra[static_cast<size_t>(k)] : nullptr;
    };
    QVector<int> tracks = m_tracks;
    const auto colorOf = [&](int k) {
        if (k >= kTraceBase) {
            const Trace &trace = r.traces[static_cast<size_t>(k - kTraceBase)];
            return trace.track >= 0 ? m_session->color(trace.track) : (trace.band ? pal.color(QPalette::Highlight) : pal.color(QPalette::Text));
        }
        return m_session->color(k);
    };
    const auto nameOf = [&](int k) {
        return k >= kTraceBase ? r.traces[static_cast<size_t>(k - kTraceBase)].label : QString::number(k);
    };

    if (traceMode) {
        for (size_t i = 0; i < r.traces.size(); ++i) {
            tracks.push_back(kTraceBase + static_cast<int>(i));
        }
    } else if (tracks.isEmpty()) {
        for (int k = 0; k < m_session->trackCount(); ++k) {
            if (m_session->visible(k)) {
                tracks.push_back(k);
            }
        }
    }

    // The level axis. Normalised: the highest peak in view at 90% of the height, and the axis as wide as the curve needs
    // to fill the graph. Not: the gain that was set, and the top the view was left with.
    // A gain of 1 is 110 dB over the height the graph had when it was first drawn (or reset); taller or shorter than that,
    // the scale stays and it covers more or fewer decibels.
    if (m_refHeight <= 0.0) {
        m_refHeight = h;
    }
    double spanDb = kSpanDb * (h / m_refHeight) / m_gain;
    double topDb = m_topDb;
    if (m_normalised) {
        float peak = -400.0F;
        std::vector<float> levels;
        for (int k : tracks) {
            const rfx::Spectrum *sp = spectrumOf(k);
            if (!sp) {
                continue;
            }
            const rfx::Spectrum &s = *sp;
            if (s.db.empty() || s.binHz <= 0.0) {
                continue;
            }
            // (without the first bins, the constant part of the signal)
            const size_t first = std::max<size_t>(2, static_cast<size_t>(std::floor(lo / s.binHz)));
            const size_t last = std::min(s.db.size() - 1, static_cast<size_t>(std::ceil((lo + span) / s.binHz)));
            const size_t stride = std::max<size_t>(1, (last > first ? last - first : 1) / 2048);
            for (size_t b = first; b <= last; b += stride) {
                peak = std::max(peak, s.db[b]);
                levels.push_back(s.db[b]);
            }
        }
        if (!levels.empty()) {
            std::nth_element(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(levels.size() / 10), levels.end());
            const double floor = static_cast<double>(levels[levels.size() / 10]);
            spanDb = std::clamp((static_cast<double>(peak) - floor) / (kPeakAt - kFloorAt), kLeastSpanDb, kMostSpanDb);
            topDb = static_cast<double>(peak) + (1.0 - kPeakAt) * spanDb;
            m_topDb = topDb;
            // What the gain comes to, for the number beside the graph.
            const double gain = kSpanDb * (h / m_refHeight) / spanDb;
            if (m_lastGain <= 0.0 || std::fabs(gain - m_lastGain) > 0.005 * m_lastGain) {
                m_lastGain = gain;
                m_gain = gain;
                QMetaObject::invokeMethod(this, [this, gain] { emit gainChanged(gain); }, Qt::QueuedConnection);
            }
        }
    }
    m_spanDb = spanDb;
    const double bottomDb = topDb - spanDb;
    const auto yOf = [&](double db) { return std::clamp((topDb - db) / (topDb - bottomDb) * h, 0.0, static_cast<double>(h)); };

    QColor grid = pal.color(QPalette::Mid);
    grid.setAlpha(150);
    p.setFont(font());
    const int fh = p.fontMetrics().height();
    // The grid and the marks of the axes, in the margins: levels on the left, frequencies at the bottom.
    {
        const double step = niceStep(spanDb, kMarks);
        for (double db = std::floor(topDb / step) * step; db >= bottomDb; db -= step) {
            if (db > topDb) {
                continue;
            }
            const int y = static_cast<int>(std::lround(yOf(db)));
            p.setPen(QPen(grid, 1));
            p.drawLine(L, y, w, y);
            drawGutterLabel(p, QRect(0, y - fh / 2, L - 6, fh), Qt::AlignRight | Qt::AlignVCenter, QString::number(db, 'f', 0) + QStringLiteral(" dB"));
        }
        const double stepHz = niceStep(span, kMarks);
        const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(stepHz / 1e6))), 0, 4);
        for (double hz = std::ceil(lo / stepHz) * stepHz; hz <= lo + span; hz += stepHz) {
            const int x = L + static_cast<int>(std::lround((hz - lo) / span * (pw - 1)));
            p.setPen(QPen(grid, 1));
            p.drawLine(x, 0, x, h);
            drawGutterLabel(p, QRect(x - 40, h + 3, 80, fh), Qt::AlignHCenter | Qt::AlignTop, QString::number(hz / 1e6, 'f', decimals) + QStringLiteral(" MHz"));
        }
        p.setPen(QPen(grid, 1));
        p.drawRect(L, 0, pw - 1, h);
    }
    // What is drawn from here on stays inside the plot.
    p.setClipRect(QRect(L, 0, pw, h + 1));

    for (int k : tracks) {
        const rfx::Spectrum *sp = spectrumOf(k);
        if (!sp) {
            continue;
        }
        const rfx::Spectrum &s = *sp;
        if (s.db.empty()) {
            continue;
        }
        QColor c = colorOf(k);
        const bool primary = k == m_session->selected() && tracks.size() > 1;
        c.setAlpha(primary || tracks.size() == 1 ? 255 : 200);
        p.setPen(QPen(c, primary ? 1.6 : 1.0));
        // The loudest bin of each column.
        std::vector<float> peak(static_cast<size_t>(pw), -400.0F);
        for (size_t b = 0; b < s.db.size(); ++b) {
            const double hz = static_cast<double>(b) * s.binHz;
            if (hz < lo) {
                continue;
            }
            if (hz > lo + span) {
                break;
            }
            const int x = std::min(pw - 1, static_cast<int>((hz - lo) / span * (pw - 1)));
            peak[static_cast<size_t>(x)] = std::max(peak[static_cast<size_t>(x)], s.db[b]);
        }
        QPointF previous;
        bool have = false;
        for (int x = 0; x < pw; ++x) {
            if (peak[static_cast<size_t>(x)] < -399.0F) {
                continue;
            }
            const QPointF pt(L + x + 0.5, yOf(peak[static_cast<size_t>(x)]));
            if (have) {
                p.drawLine(previous, pt);
            }
            previous = pt;
            have = true;
        }
    }

    if (shownRough) {
        // What is drawn is the rough spectrum of the capture, until the exact one is made.
        const QString note = QObject::tr("rough, the exact one is coming");
        drawAxisLabel(p, QPoint(L + pw - p.fontMetrics().horizontalAdvance(note) - 10, 3), note);
    }

    // The values under the mouse: the frequency, and each track's level at it.
    if (m_hover.x() >= L && m_hover.y() <= h && !m_dragging) {
        QColor hair = pal.color(QPalette::Text);
        hair.setAlpha(110);
        p.setPen(QPen(hair, 1, Qt::DotLine));
        p.drawLine(QPointF(m_hover.x() + 0.5, 0), QPointF(m_hover.x() + 0.5, h));
        const double hz = lo + static_cast<double>(m_hover.x() - L) / std::max(1, pw - 1) * span;
        QVector<ReadoutLine> lines;
        lines.push_back({QObject::tr("%1 MHz").arg(QString::number(hz / 1e6, 'f', 3)), QColor()});
        for (int k : tracks) {
            const rfx::Spectrum *sp = spectrumOf(k);
            if (!sp) {
                continue;
            }
            const rfx::Spectrum &s = *sp;
            const size_t bin = static_cast<size_t>(std::llround(hz / std::max(1e-9, s.binHz)));
            if (s.db.empty() || bin >= s.db.size()) {
                continue;
            }
            lines.push_back({QStringLiteral("%1: %2 dB").arg(nameOf(k)).arg(QString::number(static_cast<double>(s.db[bin]), 'f', 1)), colorOf(k)});
        }
        drawReadout(p, QRect(L, 0, pw, h), m_hover, lines);
    }
}

void SpectrumView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_dragging = true;
    m_dragFrom = event->position().toPoint();
    m_dragLow = m_low;
    m_dragTop = m_topDb;
    setCursor(Qt::ClosedHandCursor);
}

void SpectrumView::mouseMoveEvent(QMouseEvent *event)
{
    m_hover = event->position().toPoint();
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        // The view moves with the mouse: in frequency, and in level when it is not normalised.
        const QPoint d = m_hover - m_dragFrom;
        const double range = m_high - m_low;
        const double low = std::clamp(m_dragLow - static_cast<double>(d.x()) / std::max(1, width() - kGutterLeft) * range, 0.0, 1.0 - range);
        m_low = low;
        m_high = low + range;
        if (!m_normalised) {
            m_topDb = m_dragTop + static_cast<double>(d.y()) / std::max(1, height() - kGutterBottom) * m_spanDb;
        }
    }
    update();
}

void SpectrumView::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    unsetCursor();
}

void SpectrumView::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() == 0) {
        return;
    }
    zoomFrequency(event->angleDelta().y() > 0 ? kWheelZoom : 1.0 / kWheelZoom, (event->position().x() - kGutterLeft) / std::max(1, width() - kGutterLeft));
    event->accept();
}

void SpectrumView::leaveEvent(QEvent *)
{
    m_hover = QPoint(-1, -1);
    update();
}
