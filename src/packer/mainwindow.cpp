/******************************************************************************
 * mainwindow.cpp
 * rf-packer - the window: the list of tracks and the panel with what is known of the one picked
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "mainwindow.h"

#include "appearancemenus.h"
#include "capture.h"
#include "collapse.h"
#include "format.h"
#include "processdialog.h"
#include "sha256.h"
#include "trackrow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kListWidth = 430;
constexpr int kInfoWidth = 330;

QColor trackColor(int k)
{
    return QColor::fromHsv((k * 47) % 360, 190, 215);
}

bool isArchiveName(const QString &path)
{
    return path.endsWith(QStringLiteral(".rfx"), Qt::CaseInsensitive);
}

QString localTime(const std::string &iso)
{
    if (iso.empty()) {
        return {};
    }
    const QDateTime t = QDateTime::fromString(QString::fromStdString(iso), Qt::ISODate);
    return t.isValid() ? t.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString::fromStdString(iso);
}

} // namespace

MainWindow::MainWindow()
{
    setWindowTitle(tr("RF-packer"));
    setAcceptDrops(true);
    {
        QSettings settings;
        m_infoShown = settings.value(QStringLiteral("infoShown"), true).toBool();
        m_appearance = new AppearanceMenus(settings.value(QStringLiteral("themeMode"), static_cast<qint32>(ThemeMode::Auto)).toInt(),
                                           settings.value(QStringLiteral("customPalette")).toString(), settings.value(QStringLiteral("styleName")).toString(), this);
    }

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // The list, as in ld-stacker-gui: a title, the lines in a frame, and the buttons under it.
    auto *left = new QWidget(central);
    left->setMinimumWidth(kListWidth);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);

    auto *top = new QHBoxLayout;
    m_processButton = new QPushButton(tr("Create RFX…"), left);
    m_processButton->setObjectName(QStringLiteral("processButton"));
    m_processButton->setToolTip(tr("Write the list as an .rfx file"));
    auto *openButton = new QPushButton(tr("Open RFX…"), left);
    openButton->setObjectName(QStringLiteral("openButton"));
    openButton->setToolTip(tr("Load the tracks of an .rfx file into the list"));
    top->addWidget(m_processButton, 1);
    top->addWidget(openButton, 1);
    m_codec = new QComboBox(left);
    m_codec->setObjectName(QStringLiteral("codecBox"));
    m_codec->addItem(tr("RFX"), static_cast<int>(Codec::Rfx));
    m_codec->addItem(tr("FLAC"), static_cast<int>(Codec::Flac));
    m_codec->addItem(tr("Raw"), static_cast<int>(Codec::Raw));
    m_codec->setToolTip(tr("How the list is written: RFX (the predictor, smallest), FLAC (an .rfx file that audio programs open, larger), or Raw (the same file with the samples as they are, largest)"));
    top->addWidget(m_codec);
    leftLayout->addLayout(top);

    // What the samples contain, for the whole list: told once for a new list, read from the file for one that was opened.
    auto *format = new QHBoxLayout;
    auto *rateLabel0 = new QLabel(tr("Sample rate"), left);
    rateLabel0->setForegroundRole(QPalette::PlaceholderText);
    m_formatRate = new QDoubleSpinBox(left);
    m_formatRate->setObjectName(QStringLiteral("formatRate"));
    m_formatRate->setRange(0.0, 1000.0);
    m_formatRate->setDecimals(4);
    m_formatRate->setSuffix(QStringLiteral(" MHz"));
    m_formatRate->setSpecialValueText(tr("unknown"));
    auto *bitsLabel = new QLabel(tr("Bit depth"), left);
    bitsLabel->setForegroundRole(QPalette::PlaceholderText);
    m_formatBits = new QSpinBox(left);
    m_formatBits->setObjectName(QStringLiteral("formatBits"));
    m_formatBits->setRange(0, 16);
    m_formatBits->setSuffix(tr(" bits"));
    m_formatBits->setSpecialValueText(tr("unknown"));
    const QString said = tr("What the samples contain. It is said once for the whole list when it is new; the one of an opened file cannot be changed.");
    m_formatRate->setToolTip(said);
    m_formatBits->setToolTip(said);
    format->addWidget(rateLabel0);
    format->addWidget(m_formatRate, 1);
    format->addWidget(bitsLabel);
    format->addWidget(m_formatBits, 1);
    leftLayout->addLayout(format);

    auto *title = new QLabel(tr("Tracks"), left);
    title->setForegroundRole(QPalette::PlaceholderText);
    leftLayout->addWidget(title);

    m_listScroll = new QScrollArea(left);
    m_listScroll->setWidgetResizable(true);
    m_listScroll->setFrameShape(QFrame::StyledPanel);
    m_listScroll->setFrameShadow(QFrame::Sunken);
    m_listHost = new QWidget;
    m_listLayout = new QVBoxLayout(m_listHost);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(0);
    m_listScroll->setWidget(m_listHost);
    leftLayout->addWidget(m_listScroll, 1);

    m_hint = new QLabel(tr("<p style=\"font-size:large\">Drag and drop captures or an .rfx file here</p><p>Captures: .s16 .r16 .s8 .r8 .lds .flac .ldf</p>"), m_listScroll);
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setForegroundRole(QPalette::PlaceholderText);

    m_summary = new QLabel(left);
    m_summary->setObjectName(QStringLiteral("summary"));
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    leftLayout->addWidget(m_summary);

    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("Add track…"), left);
    addButton->setObjectName(QStringLiteral("addButton"));
    addButton->setToolTip(tr("Add captures at the end of the list"));
    m_infoButton = new QPushButton(tr("Info"), left);
    m_infoButton->setCheckable(true);
    m_infoButton->setChecked(m_infoShown);
    m_infoButton->setToolTip(tr("Show or hide the panel with what is known of the selected track (key I)"));
    buttons->addWidget(addButton, 1);
    buttons->addWidget(m_infoButton);
    leftLayout->addLayout(buttons);
    root->addWidget(left);

    // The panel on the right: all that is known of the track picked. The name, the rate, the files that go with it and the
    // notes can be changed there.
    m_info = new QWidget(central);
    m_info->setObjectName(QStringLiteral("infoPanel"));
    m_info->setFixedWidth(kInfoWidth);
    auto *infoLayout = new QVBoxLayout(m_info);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(4);
    auto *infoHead = new QHBoxLayout;
    auto *infoLabel = new QLabel(tr("Info"), m_info);
    infoLabel->setForegroundRole(QPalette::PlaceholderText);
    infoHead->addWidget(infoLabel);
    infoHead->addStretch();
    infoLayout->addLayout(infoHead);

    auto *infoScroll = new QScrollArea(m_info);
    infoScroll->setWidgetResizable(true);
    infoScroll->setFrameShape(QFrame::StyledPanel);
    infoScroll->setFrameShadow(QFrame::Sunken);
    auto *infoBody = new QWidget;
    auto *body = new QVBoxLayout(infoBody);
    body->setContentsMargins(8, 8, 8, 8);
    body->setSpacing(6);
    m_infoTitle = new QLabel(infoBody);
    m_infoTitle->setObjectName(QStringLiteral("infoTitle"));
    m_infoTitle->setWordWrap(true);
    body->addWidget(m_infoTitle);

    auto *form = new QGridLayout;
    form->setColumnStretch(1, 1);
    auto *nameLabel = new QLabel(tr("Name"), infoBody);
    nameLabel->setForegroundRole(QPalette::PlaceholderText);
    m_name = new QLineEdit(infoBody);
    m_name->setObjectName(QStringLiteral("infoName"));
    auto *rateLabel = new QLabel(tr("Rate"), infoBody);
    rateLabel->setForegroundRole(QPalette::PlaceholderText);
    m_rate = new QDoubleSpinBox(infoBody);
    m_rate->setObjectName(QStringLiteral("infoRate"));
    m_rate->setRange(0.0, 1000.0);
    m_rate->setDecimals(6);
    m_rate->setSuffix(QStringLiteral(" MHz"));
    m_rate->setSpecialValueText(tr("unknown"));
    m_rate->setReadOnly(true);
    form->addWidget(nameLabel, 0, 0);
    form->addWidget(m_name, 0, 1);
    form->addWidget(rateLabel, 1, 0);
    form->addWidget(m_rate, 1, 1);
    body->addLayout(form);

    m_facts = new QLabel(infoBody);
    m_facts->setObjectName(QStringLiteral("infoFacts"));
    m_facts->setTextFormat(Qt::RichText);
    m_facts->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_facts->setWordWrap(true);
    body->addWidget(m_facts);

    auto *sidecarsLabel = new QLabel(tr("Files that go with it"), infoBody);
    sidecarsLabel->setForegroundRole(QPalette::PlaceholderText);
    body->addWidget(sidecarsLabel);
    m_sidecars = new QListWidget(infoBody);
    m_sidecars->setObjectName(QStringLiteral("infoSidecars"));
    m_sidecars->setMaximumHeight(80);
    body->addWidget(m_sidecars);
    auto *sidecarButtons = new QHBoxLayout;
    m_addSidecar = new QPushButton(tr("Add…"), infoBody);
    m_addSidecar->setObjectName(QStringLiteral("addSidecar"));
    m_removeSidecar = new QPushButton(tr("Remove"), infoBody);
    m_removeSidecar->setObjectName(QStringLiteral("removeSidecar"));
    sidecarButtons->addWidget(m_addSidecar);
    sidecarButtons->addWidget(m_removeSidecar);
    sidecarButtons->addStretch();
    body->addLayout(sidecarButtons);

    auto *notesLabel = new QLabel(tr("Notes"), infoBody);
    notesLabel->setForegroundRole(QPalette::PlaceholderText);
    body->addWidget(notesLabel);
    m_notes = new QPlainTextEdit(infoBody);
    m_notes->setObjectName(QStringLiteral("infoNotes"));
    m_notes->setPlaceholderText(tr("A free note, kept in the metadata of the track"));
    m_notes->setMinimumHeight(110);
    body->addWidget(m_notes, 1);
    m_deleteNotes = new QPushButton(tr("Delete note"), infoBody);
    m_deleteNotes->setObjectName(QStringLiteral("deleteNotes"));
    auto *noteButtons = new QHBoxLayout;
    noteButtons->addStretch();
    noteButtons->addWidget(m_deleteNotes);
    body->addLayout(noteButtons);
    infoScroll->setWidget(infoBody);
    infoLayout->addWidget(infoScroll, 1);
    root->addWidget(m_info);

    connect(&m_doc, &Document::changed, this, [this] {
        if (m_rebuildQueued) {
            return;
        }
        m_rebuildQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_rebuildQueued = false;
            rebuildList();
            if (!m_editing) {
                refreshInfo();
            }
        });
    });
    connect(m_codec, &QComboBox::currentIndexChanged, this, [this](int) { m_doc.setCodec(static_cast<Codec>(m_codec->currentData().toInt())); });
    connect(m_formatRate, &QDoubleSpinBox::valueChanged, this, [this](double) { if (m_doc.formatEditable()) m_doc.setFormat(m_formatRate->value() * 1e6, m_formatBits->value()); });
    connect(m_formatBits, &QSpinBox::valueChanged, this, [this](int) { if (m_doc.formatEditable()) m_doc.setFormat(m_formatRate->value() * 1e6, m_formatBits->value()); });
    connect(m_processButton, &QPushButton::clicked, this, [this] { process(); });
    connect(openButton, &QPushButton::clicked, this, &MainWindow::chooseArchive);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::chooseCaptures);
    connect(m_infoButton, &QPushButton::toggled, this, [this](bool on) { showInfo(on); });
    connect(m_name, &QLineEdit::textEdited, this, &MainWindow::editName);
    connect(m_notes, &QPlainTextEdit::textChanged, this, &MainWindow::editNotes);
    connect(m_deleteNotes, &QPushButton::clicked, this, [this] { m_notes->clear(); });
    connect(m_addSidecar, &QPushButton::clicked, this, &MainWindow::addSidecar);
    connect(m_removeSidecar, &QPushButton::clicked, this, &MainWindow::removeSidecar);
    connect(m_sidecars, &QListWidget::currentRowChanged, this, [this](int row) { m_removeSidecar->setEnabled(row >= 0 && m_name->isEnabled()); });

    buildMenus();
    m_info->setVisible(m_infoShown);
    resize(kListWidth + 16 + (m_infoShown ? kInfoWidth + 8 : 0), 640);
    rebuildList();
    refreshInfo();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Create RFX from a new list"), this, &MainWindow::newList)->setShortcut(QKeySequence::New);
    file->addAction(tr("&Open RFX…"), this, &MainWindow::chooseArchive)->setShortcut(QKeySequence::Open);
    file->addAction(tr("&Add track…"), this, &MainWindow::chooseCaptures)->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    file->addSeparator();
    file->addAction(tr("&Quit"), this, &QWidget::close)->setShortcut(QKeySequence::Quit);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    m_infoAction = view->addAction(tr("&Info panel"));
    m_infoAction->setCheckable(true);
    m_infoAction->setChecked(m_infoShown);
    m_infoAction->setShortcut(QKeySequence(Qt::Key_I));
    connect(m_infoAction, &QAction::toggled, this, [this](bool on) { showInfo(on); });
    view->addSeparator();
    connect(m_appearance, &AppearanceMenus::themeModeStored, this, [](qint32 mode) { QSettings().setValue(QStringLiteral("themeMode"), mode); });
    connect(m_appearance, &AppearanceMenus::customPaletteStored, this, [](const QString &palette) { QSettings().setValue(QStringLiteral("customPalette"), palette); });
    connect(m_appearance, &AppearanceMenus::styleNameStored, this, [](const QString &name) { QSettings().setValue(QStringLiteral("styleName"), name); });
    connect(m_appearance, &AppearanceMenus::changed, this, [] {
        for (QWidget *widget : QApplication::allWidgets()) {
            widget->update();
        }
    });
    connect(m_appearance, &AppearanceMenus::customizeRequested, this, [this](QWidget *parent) { m_appearance->customize(parent, QApplication::palette()); });
    m_appearance->install(view);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About RF-packer"), this, [this] {
        QMessageBox::about(this, tr("About RF-packer"),
                           tr("<b>RF-packer</b> %1<br>Makes an .rfx file from captures, and changes the tracks of one: add, remove, reorder, code again, edit the metadata.<br>Part of RF-tools, GPL-3.0-or-later.")
                               .arg(QCoreApplication::applicationVersion()));
    });
}

void MainWindow::showInfo(bool shown)
{
    if (shown == m_infoShown) {
        return;
    }
    const int before = width();
    m_infoShown = shown;
    QSettings().setValue(QStringLiteral("infoShown"), shown);
    const QSignalBlocker a(m_infoButton);
    const QSignalBlocker b(m_infoAction);
    m_infoButton->setChecked(shown);
    m_infoAction->setChecked(shown);
    m_info->setVisible(shown);
    centralWidget()->layout()->activate();
    layout()->activate();
    // The window grows by the width of the panel, and gets it back when the panel goes (unless it is maximised).
    if (!isMaximized() && !isFullScreen()) {
        const int delta = kInfoWidth + 8;
        resize(std::max(kListWidth + 16, before + (shown ? delta : -delta)), height());
    }
}

void MainWindow::rebuildList()
{
    while (QLayoutItem *item = m_listLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    const int n = m_doc.count();
    if (m_picked >= n) {
        m_picked = n - 1;
    }
    for (int i = 0; i < n; ++i) {
        auto *row = new TrackRow(m_doc.at(i), i + 1, m_doc.state(i), trackColor(i), i == m_picked, i == 0, i == n - 1, m_listHost);
        connect(row, &TrackRow::picked, this, [this](int number) { pick(number - 1); });
        connect(row, &TrackRow::moved, this, [this](int number, int delta) { move(number - 1, delta); });
        connect(row, &TrackRow::removeAsked, this, [this](int number) { removeTrack(number - 1); });
        connect(row, &TrackRow::restoreAsked, this, [this](int number) { m_doc.restore(number - 1); });
        m_listLayout->addWidget(row);
    }
    m_listLayout->addStretch(1);
    syncFormat();
    m_hint->setVisible(n == 0);
    m_hint->setGeometry(m_listScroll->viewport()->rect());
    m_processButton->setText(m_doc.archive().isEmpty() ? tr("Create RFX…") : tr("Process…"));
    refreshSummary();
}

void MainWindow::syncFormat()
{
    const QSignalBlocker a(m_formatRate);
    const QSignalBlocker b(m_formatBits);
    const QSignalBlocker c(m_codec);
    m_formatRate->setValue(m_doc.rateHz() / 1e6);
    m_formatBits->setValue(m_doc.bits());
    m_formatRate->setEnabled(m_doc.formatEditable());
    m_formatBits->setEnabled(m_doc.formatEditable());
    m_codec->setCurrentIndex(m_codec->findData(static_cast<int>(m_doc.codec())));
}

void MainWindow::refreshSummary()
{
    const Summary s = m_doc.summary();
    QStringList parts;
    parts << tr("%n track(s)", nullptr, s.kept + s.added);
    if (s.added) {
        parts << tr("%1 new").arg(s.added);
    }
    if (s.removed) {
        parts << tr("%1 removed").arg(s.removed);
    }
    if (s.recoded) {
        parts << tr("%1 to recode").arg(s.recoded);
    }
    if (s.edited) {
        parts << tr("%1 edited").arg(s.edited);
    }
    m_summary->setText(parts.join(QStringLiteral(" · ")));
    m_processButton->setEnabled(m_doc.count() > 0 && (m_doc.archive().isEmpty() || m_doc.isChanged()) && (s.kept + s.added) > 0);
    setWindowTitle(m_doc.archive().isEmpty() ? tr("RF-packer") : tr("RF-packer - %1").arg(QFileInfo(m_doc.archive()).fileName()));
}

void MainWindow::pick(int index)
{
    if (index < 0 || index >= m_doc.count()) {
        index = -1;
    }
    if (index == m_picked) {
        return;
    }
    m_picked = index;
    rebuildList();
    refreshInfo();
}

void MainWindow::refreshInfo()
{
    const bool has = m_picked >= 0 && m_picked < m_doc.count();
    const QSignalBlocker a(m_name);
    const QSignalBlocker b(m_rate);
    const QSignalBlocker c(m_notes);
    const QSignalBlocker d(m_sidecars);
    m_sidecars->clear();
    m_name->setEnabled(has);
    m_rate->setReadOnly(true);
    m_rate->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_rate->setEnabled(has);
    m_rate->setToolTip(tr("The rate cannot be changed once the file is made; it says what the samples contain"));
    m_notes->setEnabled(has);
    m_deleteNotes->setEnabled(has);
    m_addSidecar->setEnabled(has);
    m_removeSidecar->setEnabled(false);
    if (!has) {
        m_infoTitle->setText(tr("<i>No track selected</i>"));
        m_name->clear();
        m_rate->setValue(0.0);
        m_notes->clear();
        m_facts->clear();
        return;
    }
    const Entry &e = m_doc.at(m_picked);
    // A capture that is yet to be coded takes its name and its files from where it is; its rate and notes can be given.
    const bool existing = e.fromArchive;
    m_name->setEnabled(existing);
    m_addSidecar->setEnabled(existing);
    m_name->setToolTip(existing ? QString() : tr("The name of a new track is the name of its file"));
    m_infoTitle->setText(QStringLiteral("<b>%1</b>").arg(e.name.toHtmlEscaped()));
    m_name->setText(e.name);
    m_rate->setValue(e.rateHz / 1e6);
    if (m_notes->toPlainText() != e.notes) {
        m_notes->setPlainText(e.notes);
    }
    for (const rfx::Sidecar &sc : e.sidecars) {
        m_sidecars->addItem(QString::fromStdString(sc.name) + QStringLiteral(" (") + fmt::bytes(sc.data.size()) + QLatin1Char(')'));
    }

    const rfx::SourceMeta &sm = e.meta;
    const auto times = rfx::captureTimes(e.sidecars);
    QString captured = tr("not known");
    if (times && !times->started.empty()) {
        captured = QString::fromStdString(times->started).replace(QLatin1Char('T'), QLatin1Char(' '));
        if (!times->finished.empty()) {
            captured += QStringLiteral(" → ") + QString::fromStdString(times->finished).replace(QLatin1Char('T'), QLatin1Char(' '));
        }
        if (times->durationSeconds > 0.0) {
            captured += QStringLiteral(" (") + QString::number(times->durationSeconds, 'f', 1) + QStringLiteral(" s)");
        }
    }
    const QString fileDate = sm.fileTime ? localTime(*sm.fileTime) : tr("not known");
    QStringList rows;
    auto add = [&rows](const QString &k, const QString &v) { rows << QStringLiteral("<tr><td style=\"padding-right:10px\"><small>%1</small></td><td>%2</td></tr>").arg(k.toHtmlEscaped(), v.toHtmlEscaped()); };
    add(tr("Captured"), captured);
    add(tr("File date"), fileDate);
    add(tr("Format"), QString::fromStdString(sm.fmt));
    add(tr("Size"), fmt::bytes(sm.size));
    if (existing) {
        add(tr("Stored in .rfx"), fmt::bytes(e.packed) + QStringLiteral(" · ") + fmt::percent(e.packed, sm.size));
        add(tr("Samples"), fmt::count(static_cast<qint64>(sm.words)));
        add(tr("SHA-256"), sm.sha256.empty() ? QStringLiteral("-") : QString::fromStdString(sm.sha256).left(12) + QStringLiteral("…"));
        QStringList refs;
        for (const size_t r : sm.refs) {
            refs << QString::number(r + 1);
        }
        add(tr("Predicted from"), refs.isEmpty() ? tr("none (coded alone)") : tr("track %1").arg(refs.join(QStringLiteral(", "))));
    }
    m_facts->setText(QStringLiteral("<table>%1</table>").arg(rows.join(QString())));
}

void MainWindow::editName(const QString &text)
{
    if (m_picked < 0) {
        return;
    }
    m_editing = true;
    m_doc.edit(m_picked).name = text;
    m_infoTitle->setText(QStringLiteral("<b>%1</b>").arg(text.toHtmlEscaped()));
    m_doc.touched();
    m_editing = false;
}

void MainWindow::editNotes()
{
    if (m_picked < 0) {
        return;
    }
    m_editing = true;
    m_doc.edit(m_picked).notes = m_notes->toPlainText();
    m_doc.touched();
    m_editing = false;
}

void MainWindow::addSidecar()
{
    if (m_picked < 0) {
        return;
    }
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add files that go with the track"), QSettings().value(QStringLiteral("lastDir")).toString());
    for (const QString &path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, tr("Add"), tr("%1 cannot be read: %2").arg(path, f.errorString()));
            continue;
        }
        const QByteArray bytes = f.readAll();
        rfx::Sidecar sc;
        sc.name = QFileInfo(path).fileName().toStdString();
        sc.data.assign(bytes.begin(), bytes.end());
        rfx::Sha256 hash;
        hash.update(bytes.constData(), static_cast<size_t>(bytes.size()));
        sc.sha256 = hash.finishHex();
        m_doc.edit(m_picked).sidecars.push_back(std::move(sc));
    }
    if (!files.isEmpty()) {
        QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(files.first()).absolutePath());
        m_doc.touched();
    }
}

void MainWindow::removeSidecar()
{
    const int row = m_sidecars->currentRow();
    if (m_picked < 0 || row < 0) {
        return;
    }
    auto &list = m_doc.edit(m_picked).sidecars;
    list.erase(list.begin() + row);
    m_doc.touched();
}

void MainWindow::move(int index, int delta)
{
    m_doc.move(index, delta);
    // The picked and the open lines follow the track they are on.
    if (m_picked == index) {
        m_picked = index + delta;
    } else if (m_picked == index + delta) {
        m_picked = index;
    }
}

void MainWindow::removeTrack(int index)
{
    m_doc.remove(index);
    if (m_picked >= m_doc.count()) {
        m_picked = m_doc.count() - 1;
    }
}

bool MainWindow::confirmDiscard()
{
    if (!m_doc.isChanged()) {
        return true;
    }
    const auto answer = QMessageBox::question(this, tr("RF-packer"), tr("The list has changes that are not written. Discard them?"));
    return answer == QMessageBox::Yes;
}

void MainWindow::newList()
{
    if (!confirmDiscard()) {
        return;
    }
    m_picked = -1;
    m_doc.clear();
}

void MainWindow::chooseArchive()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open an .rfx file"), QSettings().value(QStringLiteral("lastDir")).toString(), tr("RFX archives (*.rfx)"));
    if (!path.isEmpty()) {
        openArchive(path);
    }
}

void MainWindow::chooseCaptures()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Add captures"), QSettings().value(QStringLiteral("lastDir")).toString(),
                                                            tr("Captures (*.s16 *.r16 *.s8 *.r8 *.lds *.flac *.ldf);;All files (*)"));
    if (!paths.isEmpty()) {
        addCaptures(paths);
    }
}

bool MainWindow::openArchive(const QString &path)
{
    if (!confirmDiscard()) {
        return false;
    }
    QString error;
    if (!m_doc.open(path, &error)) {
        QMessageBox::warning(this, tr("Open"), tr("%1 cannot be opened: %2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(path).absolutePath());
    m_picked = m_doc.count() > 0 ? 0 : -1;
    return true;
}

bool MainWindow::addCaptures(const QStringList &paths)
{
    bool all = true;
    for (const QString &path : paths) {
        QString error;
        if (!m_doc.addCapture(path, &error)) {
            QMessageBox::warning(this, tr("Add track"), tr("%1 cannot be added: %2").arg(QFileInfo(path).fileName(), error));
            all = false;
        }
    }
    if (!paths.isEmpty()) {
        QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(paths.first()).absolutePath());
        if (m_picked < 0 && m_doc.count() > 0) {
            m_picked = 0;
        }
    }
    return all;
}

bool MainWindow::process(const QString &output, const rfx::PackOptions *options)
{
    QString target = output;
    rfx::PackOptions opts;
    if (options) {
        opts = *options;
    }
    if (target.isEmpty()) {
        ProcessDialog dialog(m_doc.archive(), m_doc.codec(), this);
        if (dialog.exec() != QDialog::Accepted) {
            return false;
        }
        target = dialog.output();
        opts = dialog.options();
    }
    const Job job = m_doc.toJob(target, opts);
    ProgressDialog progress(job, this);
    progress.start();
    if (progress.exec() != QDialog::Accepted) {
        if (!progress.error().isEmpty()) {
            QMessageBox::critical(this, tr("RF-packer"), progress.error());
        }
        return false;
    }
    statusBar()->showMessage(tr("%1 written").arg(QFileInfo(target).fileName()), 8000);
    if (m_doc.codec() != Codec::Rfx) {
        return true;
    }
    // What was written is what the list is now.
    const int keep = m_picked;
    m_doc.open(target, nullptr);
    m_picked = std::min(keep, m_doc.count() - 1);
    return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList captures;
    QString archive;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();
        if (isArchiveName(path)) {
            archive = path;
        } else {
            captures << path;
        }
    }
    if (!archive.isEmpty() && !openArchive(archive)) {
        return;
    }
    addCaptures(captures);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (confirmDiscard()) {
        event->accept();
    } else {
        event->ignore();
    }
}

bool MainWindow::saveScreenshot(const QString &path)
{
    show();
    QApplication::processEvents();
    QTimer::singleShot(0, [] {});
    QApplication::processEvents();
    return grab().save(path);
}
