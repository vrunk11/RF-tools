/******************************************************************************
 * deviationview.cpp
 * rf-analyse - the speed deviation over time
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "deviationview.h"

#include "format.h"
#include "collapse.h"
#include "readout.h"
#include "session.h"

#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kLeftMargin = 40.0;
constexpr double kRightMargin = 6.0;
constexpr double kTopMargin = 8.0;
constexpr double kBottomMargin = 16.0;
constexpr int kTicks = 4;
// The least the axis covers, in parts per million, so that a track that does not move
// is a line and not a scale.
constexpr double kLeastRangePpm = 10.0;

// `value` rounded up to 1, 2 or 5 times a power of ten.
double niceCeil(double value)
{
    const double base = std::pow(10.0, std::floor(std::log10(std::max(value, 1e-9))));
    for (const double step : {1.0, 2.0, 5.0, 10.0}) {
        if (value <= step * base) {
            return step * base;
        }
    }
    return 10.0 * base;
}

QString signedPpm(double v)
{
    const double r = std::round(v);
    return r > 0 ? QStringLiteral("+") + QString::number(r, 'f', 0) : (r < 0 ? QString(QChar(0x2212)) + QString::number(-r, 'f', 0) : QStringLiteral("0"));
}

} // namespace

DeviationView::DeviationView(Session *session, QWidget *parent) : QWidget(parent), m_session(session)
{
    setMinimumHeight(70);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(session, &Session::changed, this, qOverload<>(&QWidget::update));
    connect(session, &Session::rfSpeedChanged, this, qOverload<>(&QWidget::update));
}

void DeviationView::setTracks(const QVector<int> &tracks)
{
    m_tracks = tracks;
    update();
}

QVector<int> DeviationView::curves() const
{
    QVector<int> out;
    if (!m_session->isOpen()) {
        return out;
    }
    // The tracks of an archive: those with a reference as the speed against it, the
    // reference itself as the line of no deviation.
    for (int k = 0; k < m_session->trackCount(); ++k) {
        const bool wanted = m_tracks.isEmpty() ? m_session->visible(k) : m_tracks.contains(k);
        if (wanted && (m_session->inArchive(k) || m_session->rfSpeed())) {
            out.push_back(k);
        }
    }
    return out;
}

double DeviationView::plotLeft() const
{
    return kLeftMargin;
}

double DeviationView::plotRight() const
{
    return width() - kRightMargin;
}

qint64 DeviationView::sampleAt(double x) const
{
    const double f = std::clamp((x - plotLeft()) / std::max(1.0, plotRight() - plotLeft()), 0.0, 1.0);
    return static_cast<qint64>(f * static_cast<double>(m_session->totalSamples()));
}

double DeviationView::xOf(qint64 sample) const
{
    return plotLeft() + static_cast<double>(sample) / static_cast<double>(std::max<qint64>(1, m_session->totalSamples())) * (plotRight() - plotLeft());
}

void DeviationView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QPalette &pal = palette();
    p.fillRect(rect(), pal.base());
    if (!m_session->isOpen()) {
        return;
    }
    const QVector<int> tracks = curves();
    if (tracks.isEmpty()) {
        return;
    }
    const double top = kTopMargin;
    const double plotH = height() - kTopMargin - kBottomMargin;
    const int columns = std::max(2, static_cast<int>(plotRight() - plotLeft()));

    // The speed of every track at every column; the reference has none to show.
    std::vector<std::vector<double>> speeds;
    double biggest = 0.0;
    for (int k : tracks) {
        std::vector<double> s(static_cast<size_t>(columns), 0.0);
        if (m_session->hasSpeed(k)) {
            const qint64 words = static_cast<qint64>(m_session->track(k).words);
            for (int c = 0; c < columns; ++c) {
                const qint64 at = static_cast<qint64>(static_cast<double>(c) / (columns - 1) * static_cast<double>(m_session->totalSamples()));
                s[static_cast<size_t>(c)] = at < words ? m_session->speedAt(k, at) : 0.0;
                biggest = std::max(biggest, std::fabs(s[static_cast<size_t>(c)]));
            }
        }
        speeds.push_back(std::move(s));
    }
    // Symmetric about zero, with a little room.
    const double range = niceCeil(std::max(kLeastRangePpm, biggest * 1.15));
    const auto yOf = [&](double v) { return top + plotH / 2.0 - v / range * plotH / 2.0; };

    p.setFont(font());
    QColor grid = pal.color(QPalette::Mid);
    grid.setAlpha(110);
    const QColor label = pal.color(QPalette::PlaceholderText);
    const double values[5] = {range, range / 2, 0.0, -range / 2, -range};
    for (const double v : values) {
        const double y = yOf(v);
        p.setPen(QPen(v == 0.0 ? pal.color(QPalette::Dark) : grid, v == 0.0 ? 1.5 : 1.0));
        p.drawLine(QPointF(plotLeft(), y), QPointF(plotRight(), y));
        p.setPen(v == 0.0 ? pal.color(QPalette::Text) : label);
        p.drawText(QRectF(0, y - 7, kLeftMargin - 4, 14), Qt::AlignRight | Qt::AlignVCenter, signedPpm(v));
    }
    // Which way is which.
    p.setPen(label);
    p.drawText(QRectF(plotLeft() + 4, top + 1, plotRight() - plotLeft() - 8, 14), Qt::AlignRight | Qt::AlignTop, QString(QChar(0x25B2)) + QStringLiteral(" ") + tr("faster"));
    p.drawText(QRectF(plotLeft() + 4, top + plotH - 15, plotRight() - plotLeft() - 8, 14), Qt::AlignRight | Qt::AlignBottom, QString(QChar(0x25BC)) + QStringLiteral(" ") + tr("slower"));
    p.setPen(QPen(grid, 1));
    for (int i = 0; i <= kTicks; ++i) {
        const double x = plotLeft() + (plotRight() - plotLeft()) * i / kTicks;
        p.drawLine(QPointF(x, top), QPointF(x, top + plotH));
        p.setPen(label);
        const QString t = QString::number(m_session->seconds(m_session->totalSamples()) * 1000.0 * i / kTicks, 'f', 0) + QStringLiteral(" ms");
        p.drawText(QPointF(std::min(x - 8, static_cast<double>(width()) - 44), height() - 3), t);
        p.setPen(QPen(grid, 1));
    }

    // The window the waveform shows.
    QColor accent = pal.color(QPalette::Highlight);
    QColor tint = accent;
    tint.setAlpha(50);
    const double wx = xOf(m_session->windowStart());
    p.fillRect(QRectF(wx, top, std::max(2.0, xOf(m_session->windowStart() + m_session->windowLength()) - wx), plotH), tint);

    for (size_t i = 0; i < tracks.size(); ++i) {
        const int k = tracks[static_cast<int>(i)];
        const QColor c = m_session->color(k);
        const std::vector<double> &s = speeds[i];
        const qint64 words = static_cast<qint64>(m_session->track(k).words);
        const int drawnColumns = std::min<int>(columns, static_cast<int>(static_cast<double>(words) / static_cast<double>(std::max<qint64>(1, m_session->totalSamples())) * columns) + 1);
        // The colour follows the height: brighter and more saturated as the speed goes up above 0, darker as it goes down below it.
        const auto shade = [&](double v) {
            // A square root, so that even a small deviation shows: the colour is at its brightest or darkest well before the edge.
            const double raw = std::clamp(v / range, -1.0, 1.0);
            const double t = (raw < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(raw), 0.5);
            float h;
            float sat;
            float val;
            float alpha;
            c.getHsvF(&h, &sat, &val, &alpha);
            if (t >= 0.0) {
                sat = static_cast<float>(sat + (1.0 - sat) * t);
                val = static_cast<float>(val + (1.0 - val) * t);
            } else {
                sat = static_cast<float>(sat * (1.0 + t * 0.3));
                val = static_cast<float>(val * (1.0 + t * 0.75));
            }
            return QColor::fromHsvF(std::max(h, 0.0f), sat, val, alpha);
        };
        QPointF previous;
        for (int col = 0; col < drawnColumns; ++col) {
            const QPointF pt(plotLeft() + static_cast<double>(col) / (columns - 1) * (plotRight() - plotLeft()), yOf(s[static_cast<size_t>(col)]));
            if (col) {
                p.setPen(QPen(shade((s[static_cast<size_t>(col)] + s[static_cast<size_t>(col) - 1]) / 2.0), 2.0));
                p.drawLine(previous, pt);
            }
            previous = pt;
        }
    }

    const double bx = xOf(m_session->selection().center);
    p.setPen(QPen(accent, 2));
    p.drawLine(QPointF(bx, top), QPointF(bx, top + plotH));

    // The values under the mouse: the time and the deviation of each track there.
    if (m_hover.x() >= plotLeft() && m_hover.x() <= plotRight()) {
        QColor hair = pal.color(QPalette::Text);
        hair.setAlpha(110);
        p.setPen(QPen(hair, 1, Qt::DotLine));
        p.drawLine(QPointF(m_hover.x() + 0.5, top), QPointF(m_hover.x() + 0.5, top + plotH));
        const qint64 at = sampleAt(m_hover.x());
        QVector<ReadoutLine> lines;
        lines.push_back({tr("%1").arg(fmt::milliseconds(m_session->seconds(at))), QColor()});
        for (int k : tracks) {
            const double speed = m_session->speedIsZero(k) ? 0.0 : m_session->speedAt(k, at);
            const QString v = m_session->speedIsZero(k) ? QStringLiteral("0 (reference)") : signedPpm(speed) + QStringLiteral(" ppm");
            lines.push_back({QStringLiteral("%1: %2").arg(k).arg(v), m_session->color(k)});
        }
        drawReadout(p, rect(), m_hover, lines);
    }
}

void DeviationView::mousePressEvent(QMouseEvent *event)
{
    if (!m_session->isOpen() || event->button() != Qt::LeftButton) {
        return;
    }
    m_dragging = true;
    m_session->setCursor(sampleAt(event->position().x()), true);
}

void DeviationView::leaveEvent(QEvent *)
{
    m_hover = QPoint(-1, -1);
    update();
}

void DeviationView::mouseMoveEvent(QMouseEvent *event)
{
    m_hover = event->position().toPoint();
    update();
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        m_session->setCursor(sampleAt(event->position().x()), true);
    } else {
        m_dragging = false;
    }
}

DeviationBlock::DeviationBlock(Session *session, QWidget *parent) : QWidget(parent), m_session(session)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_head = new QWidget(this);
    auto *row = new QHBoxLayout(m_head);
    row->setContentsMargins(2, 0, 2, 0);
    auto *title = new QLabel(tr("Speed deviation over time"), m_head);
    title->setForegroundRole(QPalette::PlaceholderText);
    m_subtitle = new QLabel(tr("in ppm against the reference: 0 = no TBC correction, above faster, below slower"), m_head);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);
    m_subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_values = new QLabel(m_head);
    m_values->setTextFormat(Qt::RichText);
    m_values->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    row->addWidget(title);
    m_collapseButton = makeCollapseButton(m_head, [this](bool reduced) {
        emit reductionChanging(reduced, m_viewFrame->isVisible() ? m_viewFrame->height() : 0);
        m_collapsed = reduced;
        refresh();
    });
    row->addWidget(m_collapseButton);
    row->addWidget(m_subtitle);
    row->addStretch();
    m_rfBox = new QCheckBox(tr("From RF"), m_head);
    m_rfBox->setObjectName(QStringLiteral("rfSpeedBox"));
    m_rfBox->setToolTip(tr("Measure the speed from the RF itself instead of reading it from the stored lag: the lines of every track are located from its signal (the pass 0 of ld-decode), and the speed is the spacing of the lines against the nominal one. It works for the reference and for a capture on its own, with or without a stored curve."));
    m_rfBox->setChecked(session->rfSpeed());
    connect(m_rfBox, &QCheckBox::toggled, this, [this](bool on) { m_session->setRfSpeed(on); });
    row->addWidget(m_rfBox);
    row->addWidget(m_values);
    layout->addWidget(m_head);
    m_separator = makeSeparator(this);
    layout->addWidget(m_separator);

    m_view = new DeviationView(session, this);
    m_viewFrame = sunkenFrame(m_view, this);
    layout->addWidget(m_viewFrame, 1);

    m_unavailable = new QLabel(this);
    m_unavailable->setForegroundRole(QPalette::PlaceholderText);
    m_unavailable->setContentsMargins(10, 4, 10, 4);
    layout->addWidget(m_unavailable);

    connect(session, &Session::changed, this, &DeviationBlock::refresh);
    connect(session, &Session::rfSpeedChanged, this, [this] {
        const QSignalBlocker block(m_rfBox);
        m_rfBox->setChecked(m_session->rfSpeed());
        refresh();
    });
    connect(session, &Session::opened, this, &DeviationBlock::refresh);
    refresh();
}

// The height the block has at least when it shows its graph (reduced, it is its title bar).
void DeviationBlock::setExpandedMinimum(int height)
{
    m_expandedMinimum = height;
    refresh();
}

void DeviationBlock::setTracks(const QVector<int> &tracks)
{
    m_tracks = tracks;
    m_view->setTracks(tracks);
    refresh();
}

void DeviationBlock::refresh()
{
    const QVector<int> curves = m_view->curves();
    const bool any = !curves.isEmpty();
    m_rfBox->setEnabled(m_session->isOpen());
    // The title bar is always there; without a curve it says why, and with the block reduced it is all there is.
    m_head->setVisible(true);
    m_viewFrame->setVisible(any && !m_collapsed);
    m_separator->setVisible(!(any && !m_collapsed));
    m_unavailable->setVisible(false);
    m_collapseButton->setEnabled(any);
    QString subtitle = tr("in ppm against the reference: 0 = no TBC correction, above faster, below slower");
    if (m_session->rfSpeed()) {
        subtitle = m_session->rfSpeedBusy() ? tr("working out the speed from the RF2026") : tr("in ppm from the RF: the line spacing against the nominal one, above faster, below slower");
    }
    m_subtitle->setText(any ? subtitle
                            : tr("not applicable: %1").arg(m_session->isOpen() ? tr("a capture on its own has no reference to be compared with (tick From RF)") : tr("no file is open")));
    // Without a curve, or reduced, the block is one line, and does not take room from the others.
    const bool reduced = !any || m_collapsed;
    const int line = m_head->sizeHint().height() + 2;
    setMinimumHeight(reduced ? line : m_expandedMinimum);
    setMaximumHeight(reduced ? line : QWIDGETSIZE_MAX);
    if (!any) {
        m_values->clear();
        return;
    }
    QStringList values;
    const qint64 at = m_session->selection().center;
    for (int k : curves) {
        const QString v = m_session->speedIsZero(k) ? QStringLiteral("0 ref") : signedPpm(m_session->speedAt(k, at)) + QStringLiteral(" ppm");
        values << QStringLiteral("<span style=\"color:%1\">&#9632;</span> %2 %3").arg(m_session->color(k).name()).arg(k).arg(v);
    }
    m_values->setText(values.join(QStringLiteral(" &nbsp; ")));
}
