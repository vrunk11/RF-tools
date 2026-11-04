/******************************************************************************
 * metadatapanel.cpp
 * rf-analyse - what the file says of the selected source
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "metadatapanel.h"

#include "format.h"
#include "session.h"

#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHeaderView>
#include <QPainter>
#include <QScrollBar>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kFieldHeight = 38;

// A field: its name small above, its value below in a fixed-width font, cut with
// an ellipsis (the whole value is the tooltip).
class FieldWidget : public QWidget
{
public:
    FieldWidget(const QString &key, const QString &value, QWidget *parent) : QWidget(parent), m_key(key), m_value(value)
    {
        setToolTip(value);
        setMinimumHeight(kFieldHeight);
    }

    QSize sizeHint() const override { return {120, kFieldHeight}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        p.setFont(small);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QRect(0, 3, width(), 14), Qt::AlignLeft | Qt::AlignVCenter, m_key);
        const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        p.setFont(mono);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QRect(0, 18, width(), 16), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(mono).elidedText(m_value, Qt::ElideRight, width()));
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

private:
    QString m_key;
    QString m_value;
};

} // namespace

MetadataPanel::MetadataPanel(Session *session, QWidget *parent) : QTreeWidget(parent), m_session(session)
{
    setColumnCount(1);
    setHeaderHidden(true);
    setRootIsDecorated(true);
    setUniformRowHeights(false);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::NoFocus);
    setIndentation(0);
    header()->setStretchLastSection(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(session, &Session::opened, this, &MetadataPanel::rebuild);
    connect(session, &Session::closed, this, &MetadataPanel::rebuild);
    connect(session, &Session::changed, this, &MetadataPanel::rebuild);
    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) { m_expanded[item->text(0)] = true; });
    connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) { m_expanded[item->text(0)] = false; });
}

void MetadataPanel::rebuild()
{
    const int scroll = verticalScrollBar()->value();
    clear();
    if (!m_session->isOpen()) {
        return;
    }
    const int k = m_session->selected();
    const rfx::SourceMeta &sm = m_session->track(k);
    const double rate = sm.rateHz > 0 ? sm.rateHz : m_session->rateHz();
    const uint64_t stored = m_session->storedBytes(k);
    const rfx::LagTable *table = m_session->lagTable(k);

    // The speed against the reference, in ppm: 0 for the reference itself, and nothing for
    // a capture on its own. The lag it comes from is shown too.
    QString speed = tr("n/a");
    QString speedRange = tr("n/a");
    QString lag = tr("n/a");
    if (m_session->isReference(k)) {
        speed = tr("0 ppm (the reference)");
        speedRange = tr("0");
        lag = tr("0 (the reference)");
    } else if (table && !table->lag.empty()) {
        const auto ppm = [](double v) { return (std::round(v) > 0 ? QStringLiteral("+") : QString()) + QString::number(std::round(v), 'f', 0); };
        speed = ppm(m_session->speedAt(k, m_session->selection().center)) + tr(" ppm");
        double lo = 1e18;
        double hi = -1e18;
        const qint64 words = static_cast<qint64>(sm.words);
        for (int i = 0; i < 400; ++i) {
            const double v = m_session->speedAt(k, words * i / 399);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        speedRange = ppm(lo) + tr(" to ") + ppm(hi) + tr(" ppm");
        lag = fmt::signedNumber(table->lagAt(m_session->selection().center)) + tr(" samples");
    }
    // The references, among all the tracks.
    QString refs;
    for (size_t i = 0; i < sm.refs.size(); ++i) {
        refs += (i ? QStringLiteral(", ") : QString()) + QString::number(m_session->firstTrackOfSource(k) + static_cast<int>(sm.refs[i]));
    }
    QString weights;
    for (size_t i = 0; i < sm.weights.size(); ++i) {
        weights += (i ? QStringLiteral(", ") : QString()) + QString::number(static_cast<double>(sm.weights[i]), 'f', 2);
    }
    QString predictor = sm.refs.empty() ? tr("solo %1").arg(sm.solo ? sm.solo : sm.own) : tr("own %1, taps %2").arg(sm.own).arg(sm.taps);
    if (sm.ntop) {
        predictor += tr(", ntop %1").arg(sm.ntop);
    }

    struct Row
    {
        QString key;
        QString value;
    };
    struct Section
    {
        QString name;
        QVector<Row> rows;
    };
    QVector<Section> sections = {
        {tr("File"),
         {{tr("Name"), QString::fromStdString(sm.name)},
          {tr("Size"), fmt::bytes(sm.size) + QStringLiteral(" (") + fmt::count(static_cast<qint64>(sm.size)) + QLatin1Char(')')},
          {tr("Stored in .rfx"), fmt::bytes(stored) + QStringLiteral(" · ") + fmt::percent(stored, sm.size)},
          {tr("SHA-256"), sm.sha256.empty() ? QStringLiteral("-") : QString::fromStdString(sm.sha256).left(12) + QStringLiteral("…")},
          {tr("Sidecar"), sm.sidecars.empty() ? tr("none") : QString::fromStdString(sm.sidecars.front().name)}}},
        {tr("Samples"),
         {{tr("Duration"), fmt::duration(static_cast<double>(sm.words) / rate)},
          {tr("Count"), fmt::count(static_cast<qint64>(sm.words))},
          {tr("Rate"), sm.rateHz > 0 ? fmt::megahertz(sm.rateHz) : tr("unknown (%1)").arg(fmt::megahertz(m_session->rateHz()))},
          {tr("Format"), QString::fromStdString(sm.fmt)},
          {tr("Shift / mean"), QString::number(sm.shift) + QStringLiteral(" / ") + QString::number(sm.mean)},
          {tr("Bits carried"), QString::number(m_session->bits(k))}}},
        {tr("Coding"),
         {{tr("Order"), QString::number(k - m_session->firstTrackOfSource(k))},
          {tr("References"), refs.isEmpty() ? tr("none") : refs},
          {tr("Weights"), weights.isEmpty() ? QStringLiteral("-") : weights},
          {tr("Predictor"), predictor},
          {tr("Speed at the bar"), speed},
          {tr("Speed range"), speedRange},
          {tr("Lag at the bar"), lag},
          {tr("Origin"), sm.origin ? QString::fromStdString(*sm.origin) : QStringLiteral("-")}}}};

    // A raw capture is stored as it is, hashed by nobody, and coded by nothing: those
    // fields and the section of the coding are the archive's.
    if (!m_session->inArchive(k)) {
        sections[0].rows.remove(2, 2);
        sections.removeLast();
    }

    QFont bold = font();
    bold.setBold(true);
    for (const Section &s : sections) {
        auto *top = new QTreeWidgetItem(this, {s.name});
        top->setFont(0, bold);
        top->setSizeHint(0, QSize(0, 24));
        for (const Row &r : s.rows) {
            auto *item = new QTreeWidgetItem(top);
            item->setSizeHint(0, QSize(0, kFieldHeight));
            setItemWidget(item, 0, new FieldWidget(r.key, r.value, this));
        }
        top->setExpanded(m_expanded.value(s.name, true));
    }
    verticalScrollBar()->setValue(scroll);
}
