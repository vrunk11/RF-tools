/******************************************************************************
 * capturedialog.cpp
 * rf-analyse - what a raw capture does not say of itself
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "capturedialog.h"

#include "capture.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QDir>
#include <QSettings>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace {

constexpr double kDefaultRateMhz = 40.0;
constexpr double kHighestRateMhz = 1000.0;
constexpr int kFewestBits = 5;
constexpr int kMostBits = 16;
constexpr int kLdsBits = 10;
constexpr int kEightBits = 8;

enum Column { FileColumn, FormatColumn, RateColumn, BitsColumn, ColumnCount };

struct FormatChoice
{
    const char *label;
    rfx::SampleFormat format;
};

const FormatChoice kFormats[] = {
    {"s16: signed 16-bit (.s16)", rfx::SampleFormat::S16},
    {"r16: unsigned 16-bit", rfx::SampleFormat::U16},
    {"s8: signed 8-bit", rfx::SampleFormat::S8},
    {"r8: unsigned 8-bit", rfx::SampleFormat::U8},
    {"lds: packed 10-bit (.lds)", rfx::SampleFormat::Lds},
};

} // namespace

QStringList captureFilters()
{
    return {QObject::tr("RF files (*.rfx *.s16 *.r16 *.s8 *.r8 *.lds *.flac *.ldf)"), QObject::tr("RFX archives (*.rfx)"),
            QObject::tr("Raw captures (*.s16 *.r16 *.s8 *.r8 *.lds)"), QObject::tr("Compressed captures (*.flac *.ldf)"), QObject::tr("All files (*)")};
}

bool isCompressedCapture(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("flac") || suffix == QLatin1String("ldf");
}

bool isArchiveFile(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("rfx"), Qt::CaseInsensitive) == 0;
}

bool isRfFile(const QString &path)
{
    static const QStringList suffixes = {QStringLiteral("rfx"), QStringLiteral("s16"), QStringLiteral("r16"), QStringLiteral("s8"), QStringLiteral("r8"), QStringLiteral("lds"), QStringLiteral("flac"), QStringLiteral("ldf")};
    const QFileInfo info(path);
    return info.isFile() && suffixes.contains(info.suffix().toLower());
}

CaptureDialog::CaptureDialog(const QStringList &paths, QWidget *parent) : QDialog(parent), m_paths(paths)
{
    setWindowTitle(tr("Capture format"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(paths.size() == 1 ? tr("<b>%1</b> is not an .rfx archive: a capture says nothing of its format, so tell it.").arg(QFileInfo(paths.first()).fileName().toHtmlEscaped())
                                               : tr("These files are not .rfx archives: a capture says nothing of its format, so tell it."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_table = new QTableWidget(paths.size(), ColumnCount, this);
    m_table->setHorizontalHeaderLabels({tr("File"), tr("Format"), tr("Sample rate"), tr("Bits carried")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(FileColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(FormatColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(RateColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(BitsColumn, QHeaderView::ResizeToContents);
    layout->addWidget(m_table);

    QSettings settings;
    const int lastFormat = settings.value(QStringLiteral("capture/format"), 0).toInt();
    const double lastRate = settings.value(QStringLiteral("capture/rateMhz"), kDefaultRateMhz).toDouble();
    bool anySidecar = false;
    bool anyCompressed = false;
    for (int row = 0; row < paths.size(); ++row) {
        const QString &path = paths[row];
        auto *name = new QTableWidgetItem(QFileInfo(path).fileName());
        name->setToolTip(path);
        m_table->setItem(row, FileColumn, name);

        auto *format = new QComboBox(m_table);
        for (const FormatChoice &c : kFormats) {
            format->addItem(tr(c.label), static_cast<int>(c.format));
        }
        // The format from the extension; else the last one used.
        int index = std::clamp(lastFormat, 0, format->count() - 1);
        const QString suffix = QFileInfo(path).suffix().toLower();
        for (int i = 0; i < format->count(); ++i) {
            if (QString::fromLatin1(rfx::formatName(static_cast<rfx::SampleFormat>(format->itemData(i).toInt()))) == suffix) {
                index = i;
            }
        }
        format->setCurrentIndex(index);
        m_table->setCellWidget(row, FormatColumn, format);

        auto *rate = new QDoubleSpinBox(m_table);
        rate->setDecimals(3);
        rate->setRange(0.001, kHighestRateMhz);
        rate->setSuffix(tr(" MHz"));
        rate->setSingleStep(1.0);
        // The rate from the .yaml beside the capture; else the last one used.
        double mhz = lastRate;
        const std::optional<double> fromSidecar = rfx::sidecarRateHz(rfx::findSidecars(path));
        if (fromSidecar && *fromSidecar > 0) {
            mhz = *fromSidecar / 1e6;
            anySidecar = true;
        }
        rate->setValue(mhz);
        m_table->setCellWidget(row, RateColumn, rate);

        auto *bits = new QComboBox(m_table);
        bits->addItem(tr("Found from the samples"), 0);
        for (int b = kFewestBits; b <= kMostBits; ++b) {
            bits->addItem(tr("%1 bits").arg(b), b);
        }
        m_table->setCellWidget(row, BitsColumn, bits);

        connect(format, &QComboBox::currentIndexChanged, this, [this, row](int) { formatChanged(row); });
        anyCompressed = anyCompressed || isCompressedCapture(path);
        formatChanged(row);
    }
    m_table->resizeRowsToContents();

    QStringList notes;
    if (anySidecar) {
        notes << tr("The rate is the one the .yaml beside a capture gives, where there is one.");
    }
    if (anyCompressed) {
        notes << tr("A FLAC capture is decoded with ffmpeg to a temporary 16-bit file first, which takes room and a moment. Its header holds the rate in kHz only: give the real one.");
    }
    m_note = new QLabel(notes.join(QLatin1Char('\n')), this);
    m_note->setWordWrap(true);
    m_note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        remember();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // As big as its rows need (up to a dozen of them, then it scrolls), and as the user last made it.
    setSizeGripEnabled(true);
    constexpr int kRowsShown = 12;
    const int rowHeight = m_table->rowCount() > 0 ? std::max(28, m_table->rowHeight(0)) : 28;
    const int tableHeight = m_table->horizontalHeader()->sizeHint().height() + rowHeight * std::min<int>(kRowsShown, static_cast<int>(paths.size())) + 6;
    m_table->setMinimumHeight(tableHeight);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    adjustSize();
    resize(std::max(720, width()), height());
    const QSize last = settings.value(QStringLiteral("capture/dialogSize")).toSize();
    if (last.isValid() && last.width() >= 560) {
        resize(last.width(), std::max(last.height(), minimumSizeHint().height()));
    }
}

int CaptureDialog::rowCount() const
{
    return m_table->rowCount();
}

bool CaptureDialog::compressed(int row) const
{
    return isCompressedCapture(m_paths.at(row));
}

void CaptureDialog::formatChanged(int row)
{
    auto *format = qobject_cast<QComboBox *>(m_table->cellWidget(row, FormatColumn));
    auto *bits = qobject_cast<QComboBox *>(m_table->cellWidget(row, BitsColumn));
    if (!format || !bits) {
        return;
    }
    if (compressed(row)) {
        // Decoded to 16-bit samples before it is read.
        format->setCurrentIndex(0);
        format->setEnabled(false);
    }
    const auto f = static_cast<rfx::SampleFormat>(format->currentData().toInt());
    // The bits a format can carry.
    const int most = (f == rfx::SampleFormat::S8 || f == rfx::SampleFormat::U8) ? kEightBits : kMostBits;
    auto *model = qobject_cast<QStandardItemModel *>(bits->model());
    for (int i = 1; i < bits->count(); ++i) {
        if (QStandardItem *item = model ? model->item(i) : nullptr) {
            item->setEnabled(bits->itemData(i).toInt() <= most);
        }
    }
    if (f == rfx::SampleFormat::Lds) {
        bits->setCurrentIndex(bits->findData(kLdsBits));
        bits->setEnabled(false);
    } else {
        bits->setEnabled(true);
        if (bits->currentData().toInt() > most) {
            bits->setCurrentIndex(0);
        }
    }
}

rfx::CaptureSpec CaptureDialog::spec(int row) const
{
    rfx::CaptureSpec s;
    s.format = static_cast<rfx::SampleFormat>(qobject_cast<QComboBox *>(m_table->cellWidget(row, FormatColumn))->currentData().toInt());
    s.rateHz = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, RateColumn))->value() * 1e6;
    const int bits = qobject_cast<QComboBox *>(m_table->cellWidget(row, BitsColumn))->currentData().toInt();
    if (bits > 0) {
        s.bits = bits;
    }
    s.name = QFileInfo(m_paths.at(row)).fileName().toStdString();
    // A compressed capture is decoded where it is looked at, as 16-bit samples.
    s.compressed = isCompressedCapture(m_paths.at(row));
    return s;
}

void CaptureDialog::remember()
{
    QSettings().setValue(QStringLiteral("capture/dialogSize"), size());
    // What is offered next time: the rate and the format of the last row that is not
    // compressed.
    QSettings settings;
    for (int row = m_table->rowCount() - 1; row >= 0; --row) {
        if (!compressed(row)) {
            settings.setValue(QStringLiteral("capture/format"), qobject_cast<QComboBox *>(m_table->cellWidget(row, FormatColumn))->currentIndex());
            settings.setValue(QStringLiteral("capture/rateMhz"), qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, RateColumn))->value());
            return;
        }
    }
    settings.setValue(QStringLiteral("capture/rateMhz"), qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(0, RateColumn))->value());
}

QStringList rfFilesInFolder(const QString &folder)
{
    QStringList found;
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::LocaleAware);
    for (const QFileInfo &info : entries) {
        if (isRfFile(info.absoluteFilePath())) {
            found << info.absoluteFilePath();
        }
    }
    return found;
}
