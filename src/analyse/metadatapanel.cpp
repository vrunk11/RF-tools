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

#include "capture.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

constexpr int kFieldHeight = 38;

// A field: its name small above, its value below in a fixed-width font, cut with
// an ellipsis (the whole value is the tooltip).
class FieldWidget : public QWidget
{
public:
    // `lines` more than one: the value is wrapped over that many lines (what does not fit is cut; the tooltip has it all).
    FieldWidget(const QString &key, const QString &value, QWidget *parent, int lines = 1) : QWidget(parent), m_key(key), m_value(value), m_lines(lines)
    {
        setToolTip(value);
        setMinimumHeight(heightFor(lines));
    }

    static int heightFor(int lines) { return kFieldHeight + (lines - 1) * 16; }
    QSize sizeHint() const override { return {120, heightFor(m_lines)}; }

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
        if (m_lines > 1) {
            p.drawText(QRect(0, 18, width(), 16 * m_lines), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_value);
        } else {
            p.drawText(QRect(0, 18, width(), 16), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(mono).elidedText(m_value, Qt::ElideRight, width()));
        }
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

private:
    QString m_key;
    QString m_value;
    int m_lines = 1;
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
    connect(session, &Session::rfSpeedChanged, this, &MetadataPanel::rebuild);
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
    if (m_session->isReference(k) && !m_session->rfSpeed(k)) {
        lag = tr("0 (the reference)");
    } else if (m_session->rfSpeed(k) && m_session->hasSpeed(k)) {
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
        if (table) {
            lag = fmt::signedNumber(table->lagAt(m_session->selection().center)) + tr(" samples");
        }
    } else if (table && !table->lag.empty()) {
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

    // When the capture was made (the .yaml beside it, kept in the archive), when its file was last changed, and the note.
    QString captured = tr("not known");
    if (const std::optional<rfx::CaptureTimes> times = rfx::captureTimes(sm.sidecars)) {
        const QDateTime from = QDateTime::fromString(QString::fromStdString(times->started), Qt::ISODate);
        const QDateTime to = QDateTime::fromString(QString::fromStdString(times->finished), Qt::ISODate);
        captured = from.isValid() ? from.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString::fromStdString(times->started);
        if (to.isValid()) {
            captured += QStringLiteral(" \u2192 ") + (to.date() == from.date() ? to.toString(QStringLiteral("HH:mm:ss")) : to.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        }
        if (times->durationSeconds > 0) {
            captured += QStringLiteral(" (%1 s)").arg(QString::number(times->durationSeconds, 'f', 1));
        }
    }
    QString fileDate = tr("not recorded");
    if (m_session->inArchive(k)) {
        if (sm.fileTime) {
            const QDateTime when = QDateTime::fromString(QString::fromStdString(*sm.fileTime), Qt::ISODate).toLocalTime();
            fileDate = when.isValid() ? when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString::fromStdString(*sm.fileTime);
        }
    } else {
        const QDateTime when = QFileInfo(m_session->sourcePath(k)).lastModified();
        fileDate = when.isValid() ? when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : fileDate;
    }
    const QString notes = QString::fromStdString(sm.notes);

    struct Row
    {
        QString key;
        QString value;
        int lines = 1;
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
        {tr("Capture"),
         {{tr("Captured"), captured},
          {tr("File date"), fileDate},
          {tr("Standard"), sm.standard.empty() ? tr("not said") : QString::fromStdString(sm.standard).toUpper()},
          {tr("Reference"), sm.reference.empty() ? tr("not said") : QString::fromStdString(sm.reference)},
          {tr("Disc copy"), sm.copy > 0 ? QString::number(sm.copy) : tr("not said")},
          {tr("Capture no."), sm.capture > 0 ? QString::number(sm.capture) : tr("not said")},
          {tr("Face"), sm.face.empty() ? tr("not said") : QString::fromStdString(sm.face)},
          {tr("Signal"), sm.signal.empty() ? tr("not said") : (sm.signal == "yc" ? QStringLiteral("Y/C") : QStringLiteral("CVBS"))},
          {tr("Source"), QString::fromStdString(rfx::mediumName(m_session->mediumOf(k)))},
          {tr("Notes"), notes.isEmpty() ? tr("none") : notes, notes.isEmpty() ? 1 : 4}}},
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

    // What the .yaml beside the capture (or kept in the archive) says besides: one section for each of its parts that has something in it.
    {
        int at = 2;
        for (const rfx::SidecarSection &s : rfx::sidecarSections(sm.sidecars)) {
            Section section;
            section.name = s.name == "file" ? tr("Sidecar") : s.name == "capture" ? tr("Capture settings") : tr("%1").arg(QString::fromStdString(s.name).replace(QLatin1Char('_'), QLatin1Char(' ')).replace(0, 1, QString::fromStdString(s.name).left(1).toUpper()));
            for (const auto &row : s.rows) {
                if (row.first == "started" || row.first == "finished" || row.first == "duration_seconds" || row.first == "file") {
                    continue;
                }
                QString key = QString::fromStdString(row.first).replace(QLatin1Char('_'), QLatin1Char(' '));
                key.replace(0, 1, key.left(1).toUpper());
                section.rows.push_back({key, QString::fromStdString(row.second)});
            }
            if (!section.rows.isEmpty()) {
                sections.insert(at++, section);
            }
        }
    }

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
            item->setSizeHint(0, QSize(0, FieldWidget::heightFor(r.lines)));
            setItemWidget(item, 0, new FieldWidget(r.key, r.value, this, r.lines));
        }
        top->setExpanded(m_expanded.value(s.name, true));
    }
    verticalScrollBar()->setValue(scroll);
}
