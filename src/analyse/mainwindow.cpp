/******************************************************************************
 * mainwindow.cpp
 * rf-analyse - the window
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "mainwindow.h"

#include "archive.h"
#include "capture.h"
#include "capturedialog.h"
#include "deviationview.h"
#include "error.h"
#include "ffmpeg.h"
#include "format.h"
#include "appearancemenus.h"
#include "collapse.h"
#include "metadatapanel.h"
#include "session.h"
#include "verify.h"
#include "spectrumview.h"
#include "standarddialog.h"
#include "statspanel.h"
#include "trackeditdialog.h"
#include "tracklist.h"
#include "waveformview.h"

#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QDoubleSpinBox>
#include <QHash>
#include <QTimer>
#include <QSet>
#include <cstdlib>
#include <optional>
#include <QMouseEvent>
#include <QUrl>
#include <QListWidget>
#include <QPushButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

namespace {

constexpr int kRecentFiles = 8;
constexpr int kSliderSteps = 1000;
// The smallest the views can be made (the window with them): the waveform with its title, and the bar of the zoom.
// The size the window is given by the compact mode, when it is larger.
constexpr int kCompactWidth = 900;
constexpr int kCompactHeight = 560;
constexpr int kViewMinimumWidth = 500;
constexpr int kViewMinimumHeight = 150;
constexpr int kMetadataWidth = 260;
constexpr int kTrackListWidth = 270;
// The amplitude of the graph: what the buttons multiply it by, and its limits.
constexpr double kAmplitudeStep = 1.25;
constexpr double kAmplitudeLowest = 0.25;
constexpr double kAmplitudeHighest = 1024.0;
constexpr int kFftSizes[] = {1024, 4096, 16384, 65536, 262144, 1048576};
constexpr int kTabCombined = 0;
constexpr int kTabIndividual = 1;

// A view in a sunken frame, as the picture and the plot of ld-stacker-gui are: what gives the panels their depth.
QFrame *sunken(QWidget *inner, QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setFrameShadow(QFrame::Sunken);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    inner->setParent(frame);
    layout->addWidget(inner);
    return frame;
}

QWidget *rule(QWidget *parent)
{
    auto *line = new QWidget(parent);
    line->setFixedHeight(1);
    line->setAutoFillBackground(true);
    QPalette p = line->palette();
    p.setColor(QPalette::Window, parent->palette().color(QPalette::Mid));
    line->setPalette(p);
    return line;
}

// A heading row: a title in the colour of the placeholders, as the lists of ld-stacker-gui have, then whatever `fill` puts in it.
QWidget *heading(const QString &title, QWidget *parent, QLabel **extra = nullptr)
{
    auto *w = new QWidget(parent);
    auto *row = new QHBoxLayout(w);
    row->setContentsMargins(2, 0, 2, 0);
    auto *t = new QLabel(title, w);
    t->setForegroundRole(QPalette::PlaceholderText);
    row->addWidget(t);
    if (extra) {
        *extra = new QLabel(w);
        (*extra)->setForegroundRole(QPalette::PlaceholderText);
        (*extra)->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        row->addStretch();
        row->addWidget(*extra);
    }
    return w;
}

// A button with an icon of ld-analyse and ld-stacker-gui, and its name as the tip
QToolButton *iconButton(const QString &icon, const QString &tip, QWidget *parent, bool repeat = false)
{
    auto *b = new QToolButton(parent);
    b->setIcon(QIcon(QStringLiteral(":/icons/Graphics/") + icon));
    b->setIconSize(QSize(22, 22));
    b->setToolTip(tip);
    b->setAutoRepeat(repeat);
    b->setAutoRepeatDelay(500);
    b->setAutoRepeatInterval(30);
    return b;
}

} // namespace

MainWindow::MainWindow() : m_session(new Session(this))
{
    {
        QSettings settings;
        const char *keys[] = {"panelWave", "panelSpectrum", "panelDeviation", "panelStats"};
        for (int p = 0; p < kPanelCount; ++p) {
            m_showPanel[p] = settings.value(QLatin1String(keys[p]), true).toBool();
        }
        m_followSize = settings.value(QStringLiteral("gainOnResize"), true).toBool();
        m_normaliseFft = settings.value(QStringLiteral("normaliseFft"), true).toBool();
        m_compact = settings.value(QStringLiteral("compactMode"), false).toBool();
    }
    Q_INIT_RESOURCE(rf_analyse_resources);
    // Colour theme and widget style, as in ld-analyse and ld-stacker-gui: View > Theme and View > Style, kept in the settings.
    {
        QSettings settings;
        m_appearance = new AppearanceMenus(settings.value(QStringLiteral("themeMode"), static_cast<qint32>(ThemeMode::Auto)).toInt(),
                                           settings.value(QStringLiteral("customPalette")).toString(), settings.value(QStringLiteral("styleName")).toString(), this);
    }
    setAcceptDrops(true);
    resize(1280, 820);

    m_pages = new QStackedWidget(this);
    setCentralWidget(m_pages);

    // With nothing open: the empty list of tracks, with its button, and what can be done to fill it.
    auto *empty = new QWidget(m_pages);
    auto *emptyLayout = new QHBoxLayout(empty);
    emptyLayout->setContentsMargins(8, 8, 8, 8);
    emptyLayout->setSpacing(8);
    auto *emptySide = new QWidget(empty);
    emptySide->setFixedWidth(kTrackListWidth);
    auto *emptySideLayout = new QVBoxLayout(emptySide);
    emptySideLayout->setContentsMargins(0, 0, 0, 0);
    emptySideLayout->setSpacing(4);
    auto *emptyTitle = new QLabel(tr("Tracks"), emptySide);
    emptyTitle->setForegroundRole(QPalette::PlaceholderText);
    auto *emptyList = new QListWidget(emptySide);
    emptyList->setFrameShape(QFrame::StyledPanel);
    emptyList->setFrameShadow(QFrame::Sunken);
    auto *emptyOpen = new QPushButton(tr("Open source…"), emptySide);
    connect(emptyOpen, &QPushButton::clicked, this, &MainWindow::chooseFile);
    emptySideLayout->addWidget(emptyTitle);
    emptySideLayout->addWidget(emptyList, 1);
    emptySideLayout->addWidget(emptyOpen);
    auto *emptyPane = new QFrame(empty);
    emptyPane->setFrameShape(QFrame::StyledPanel);
    emptyPane->setFrameShadow(QFrame::Sunken);
    auto *emptyPaneLayout = new QVBoxLayout(emptyPane);
    auto *hintLabel = new QLabel(tr("<p style=\"font-size:large\">Drag and drop one or several RF files here</p><p>.rfx archives and captures (.s16 .r16 .s8 .r8 .lds .flac .ldf);<br>captures on their own are asked for their format and rate.</p><p>Or use <b>Open source…</b>, or <b>File &gt; Open RF file…</b> (Ctrl+O).</p>"), emptyPane);
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setForegroundRole(QPalette::PlaceholderText);
    emptyPaneLayout->addWidget(hintLabel);
    emptyLayout->addWidget(emptySide);
    emptyLayout->addWidget(emptyPane, 1);
    m_pages->addWidget(empty);

    // The tracks | the metadata of the selected one | the views.
    auto *split = new QSplitter(Qt::Horizontal, m_pages);
    split->setChildrenCollapsible(false);

    m_tracks = new TrackList(m_session, split);
    m_tracks->setObjectName(QStringLiteral("trackList"));
    auto *left = new QWidget(split);
    auto *leftLayout = new QVBoxLayout(left);
    // As the side panels of ld-stacker-gui: room round them, a title in the colour of the placeholders, and the list in a frame.
    leftLayout->setContentsMargins(8, 8, 8, 8);
    leftLayout->setSpacing(4);
    auto *tracksTitle = new QLabel(tr("Tracks"), left);
    tracksTitle->setForegroundRole(QPalette::PlaceholderText);
    m_tracksHeadLayout = new QHBoxLayout;
    m_tracksHeadLayout->setContentsMargins(0, 0, 0, 0);
    m_tracksHeadLayout->addWidget(tracksTitle);
    m_tracksHeadLayout->addStretch();
    leftLayout->addLayout(m_tracksHeadLayout);
    m_tracks->setFrameShape(QFrame::StyledPanel);
    m_tracks->setFrameShadow(QFrame::Sunken);
    leftLayout->addWidget(m_tracks, 1);
    // As the Add source and Info buttons under the list of ld-stacker-gui.
    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("Open source…"), left);
    addButton->setToolTip(tr("Add more RF files (.rfx or captures) to the ones that are open"));
    m_infoButton = new QPushButton(tr("Info"), left);
    m_infoButton->setCheckable(true);
    m_infoButton->setChecked(true);
    m_infoButton->setToolTip(tr("Show or hide the panel with what is known of the selected track (key I)"));
    m_verifyAll = new QPushButton(tr("Check"), left);
    m_verifyAll->setObjectName(QStringLiteral("verifyAll"));
    m_verifyAll->setToolTip(tr("Check every track of the .rfx files that are open: decode it and compare it with the SHA-256 the file keeps of its samples"));
    buttons->addWidget(addButton, 1);
    buttons->addWidget(m_verifyAll);
    buttons->addWidget(m_infoButton);
    connect(m_verifyAll, &QPushButton::clicked, this, [this] { verifyTracks(std::nullopt); });
    leftLayout->addLayout(buttons);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addFiles);
    connect(m_infoButton, &QPushButton::toggled, this, [this](bool on) {
        if (on != m_panelShown) {
            showMetadata(on, true);
        }
    });
    m_listColumn = left;
    left->setMinimumWidth(kTrackListWidth);
    left->setMaximumWidth(kTrackListWidth + 120);

    m_panel = new QWidget(split);
    auto *panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setContentsMargins(8, 8, 8, 8);
    panelLayout->setSpacing(4);
    auto *panelHead = new QWidget(m_panel);
    auto *panelRow = new QHBoxLayout(panelHead);
    m_panelHeadLayout = panelRow;
    panelRow->setContentsMargins(0, 0, 0, 0);
    auto *panelTitle = new QLabel(tr("Source"), panelHead);
    panelTitle->setForegroundRole(QPalette::PlaceholderText);
    m_panelTrack = new QLabel(panelHead);
    m_panelTrack->setForegroundRole(QPalette::PlaceholderText);
    auto *panelClose = new QToolButton(panelHead);
    panelClose->setText(QString(QChar(0x2715)));
    panelClose->setAutoRaise(true);
    panelClose->setToolTip(tr("Hide the metadata"));
    panelRow->addWidget(panelTitle);
    panelRow->addStretch();
    panelRow->addWidget(m_panelTrack);
    panelRow->addWidget(panelClose);
    m_metadata = new MetadataPanel(m_session, m_panel);
    m_metadata->setObjectName(QStringLiteral("metadataPanel"));
    m_metadata->setFrameShape(QFrame::StyledPanel);
    m_metadata->setFrameShadow(QFrame::Sunken);
    panelLayout->addWidget(panelHead);
    panelLayout->addWidget(m_metadata, 1);
    m_verifyTrack = new QPushButton(tr("Check this track"), m_panel);
    m_verifyTrack->setObjectName(QStringLiteral("verifyTrack"));
    m_verifyTrack->setToolTip(tr("Decode the selected track and compare it with the SHA-256 the file keeps of its samples"));
    m_computeTrack = new QPushButton(tr("Compute TBC curve"), m_panel);
    m_computeTrack->setObjectName(QStringLiteral("computeTrack"));
    m_computeTrack->setToolTip(tr("Work out the TBC curve of the selected track from its RF: where its lines are, from which the speed of the disc and the time base correction are made. The list of tracks shows how far it is. File > Save TBC curve keeps it in the .rfx file"));
    auto *trackButtons = new QHBoxLayout;
    trackButtons->addWidget(m_verifyTrack);
    trackButtons->addWidget(m_computeTrack);
    m_editTrack = new QPushButton(tr("Edit…"), m_panel);
    m_editTrack->setObjectName(QStringLiteral("editTrack"));
    m_editTrack->setToolTip(tr("Say or change the name, the note, and what is known of the disc and the signal (standard, reference, copy, face, CVBS or Y/C) for the selected track: the .rfx file is written again with it, its samples copied as they are"));
    trackButtons->addWidget(m_editTrack);
    connect(m_editTrack, &QPushButton::clicked, this, [this] { editTrack(); });
    trackButtons->addStretch();
    panelLayout->addLayout(trackButtons);
    connect(m_verifyTrack, &QPushButton::clicked, this, [this] { verifyTracks(m_session->selected()); });
    connect(m_computeTrack, &QPushButton::clicked, this, [this] {
        m_session->computeCurve(m_session->selected());
        updateVerify();
    });
    m_panel->setMinimumWidth(kMetadataWidth);
    connect(panelClose, &QToolButton::clicked, this, [this] { showMetadata(false, true); });

    auto *right = new QWidget(split);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 4, 8, 8);
    rightLayout->setSpacing(0);

    // The tabs, and in the individual one the choice of the track alone or every
    // lane.
    auto *tabRow = new QWidget(right);
    auto *tabLayout = new QHBoxLayout(tabRow);
    m_tabLayout = tabLayout;
    tabLayout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabBar(tabRow);
    m_tabs->addTab(tr("Combined"));
    m_tabs->addTab(tr("Individual"));
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    tabLayout->addWidget(m_tabs);
    // The correction of the time base is of every view: its box is by the tabs, with the two that depend on it (the stack and the noise it
    // removes, which only the combined view shows).
    tabLayout->addSpacing(12);
    m_alignBox = alignBox(tabRow);
    tabLayout->addWidget(m_alignBox);
    m_stackBox = new QCheckBox(tr("Stack"), tabRow);
    m_stackBox->setObjectName(QStringLiteral("stackBox"));
    m_stackBox->setToolTip(tr("Show the stack in place of the tracks: the mean of the tracks shown once their time base is corrected (the waveform and the spectrum). Combined view only."));
    tabLayout->addWidget(m_stackBox);
    m_residueBox = new QCheckBox(tr("Noise residue"), tabRow);
    m_residueBox->setObjectName(QStringLiteral("residueBox"));
    m_residueBox->setToolTip(tr("Show the noise the stack removes in place of the tracks: what each track has more than the stack (a curve for each, or one for all of them: View > Show combined residue). Combined view only."));
    tabLayout->addWidget(m_residueBox);
    // Both need the time base corrected: ticking one ticks the box Align capture (and asks for the TBC curve it needs), unticking that unticks both.
    const auto askedWithAlign = [this](QCheckBox *box, void (Session::*set)(bool)) {
        connect(box, &QCheckBox::toggled, this, [this, box, set](bool on) {
            if (on && !m_alignBox->isChecked()) {
                m_alignBox->setChecked(true);
            }
            const bool really = on && m_alignBox->isChecked();
            if (box->isChecked() != really) {
                const QSignalBlocker block(box);
                box->setChecked(really);
            }
            (m_session->*set)(really);
        });
    };
    askedWithAlign(m_stackBox, &Session::setStackShown);
    askedWithAlign(m_residueBox, &Session::setResidueShown);
    connect(m_alignBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) {
            m_stackBox->setChecked(false);
            m_residueBox->setChecked(false);
        }
    });
    // With the stack or the residue on, the time base is corrected whatever is asked: the box Align capture is greyed. With one track alone
    // there is nothing to stack, and the two boxes are greyed.
    const auto enableBoxes = [this] {
        const bool several = m_session->trackCount() > 1;
        if (!several) {
            m_stackBox->setChecked(false);
            m_residueBox->setChecked(false);
        }
        m_stackBox->setEnabled(several);
        m_residueBox->setEnabled(several);
        m_alignBox->setEnabled(!m_stackBox->isChecked() && !m_residueBox->isChecked());
    };
    connect(m_stackBox, &QCheckBox::toggled, this, enableBoxes);
    connect(m_residueBox, &QCheckBox::toggled, this, enableBoxes);
    connect(m_session, &Session::opened, this, enableBoxes);
    connect(m_session, &Session::closed, this, enableBoxes);
    enableBoxes();
    tabLayout->addStretch();
    m_modeBox = new QWidget(tabRow);
    auto *modeLayout = new QHBoxLayout(m_modeBox);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(0);
    m_modeGroup = new QButtonGroup(this);
    const QString modes[] = {tr("Selected"), tr("All lanes")};
    for (int i = 0; i < 2; ++i) {
        auto *b = new QToolButton(m_modeBox);
        b->setText(modes[i]);
        b->setCheckable(true);
        b->setChecked(i == 0);
        m_modeGroup->addButton(b, i);
        modeLayout->addWidget(b);
    }
    tabLayout->addWidget(m_modeBox);
    // (the row is as tall in every view, whatever it shows: the graphs below do not change size with the tab)
    tabRow->setMinimumHeight(std::max({m_alignBox->sizeHint().height(), m_tabs->sizeHint().height(), m_modeBox->sizeHint().height()}) + tabLayout->contentsMargins().top() + tabLayout->contentsMargins().bottom());
    rightLayout->addWidget(tabRow);
    // The zoom: small buttons in the block of the graph (placed by placeZoomBar), not a row of their own.
    m_zoomBar = new QWidget(right);
    auto *zoomLayout = new QHBoxLayout(m_zoomBar);
    zoomLayout->setContentsMargins(0, 0, 0, 0);
    zoomLayout->setSpacing(2);
    const auto small = [this](const QString &icon, const QString &tip, bool repeat) {
        auto *b = iconButton(icon, tip, m_zoomBar, repeat);
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        return b;
    };
    auto *zoomOut = small(QStringLiteral("zoom-out.png"), tr("Lower the amplitude (the gain of the graph); the wheel with ctrl zooms in time"), true);
    auto *zoomIn = small(QStringLiteral("zoom-in.png"), tr("Raise the amplitude (the gain of the graph); the wheel with ctrl zooms in time"), true);
    auto *zoomFit = small(QStringLiteral("zoom-original.png"), tr("Back to the default: every axis at its original size (the window in time and the amplitude)"), false);
    // The size of the window in time (it changes with the wheel), the amplitude as a number, then the buttons, all at
    // the right: what changes width is on the left of what stays, so the buttons do not move.
    m_zoomLabel = new QLabel(m_zoomBar);
    m_zoomLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_zoomLabel->setForegroundRole(QPalette::PlaceholderText);
    m_zoomLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_zoomLabel->setMinimumWidth(m_zoomLabel->fontMetrics().horizontalAdvance(QStringLiteral("000,000,000 samples · 00000.0 µs")));
    zoomLayout->addWidget(m_zoomLabel);
    zoomLayout->addSpacing(8);
    zoomLayout->addWidget(new QLabel(tr("Amplitude"), m_zoomBar));
    m_amplitude = new QDoubleSpinBox(m_zoomBar);
    m_amplitude->setRange(kAmplitudeLowest, kAmplitudeHighest);
    m_amplitude->setDecimals(2);
    m_amplitude->setSingleStep(0.25);
    m_amplitude->setPrefix(QStringLiteral("x"));
    m_amplitude->setValue(1.0);
    m_amplitude->setKeyboardTracking(false);
    m_amplitude->setFixedWidth(84);
    m_amplitude->setObjectName(QStringLiteral("waveAmplitude"));
    m_amplitude->setToolTip(tr("The amplitude of the graph: 1 shows the whole range of the converter"));
    zoomLayout->addWidget(m_amplitude);
    zoomLayout->addSpacing(4);
    zoomLayout->addWidget(zoomOut);
    zoomLayout->addWidget(zoomIn);
    zoomLayout->addWidget(zoomFit);

    // Everything under the tabs is one panel, a step up from the graphs in it, as the pane of a tab widget.
    m_pane = new QFrame(right);
    m_pane->setFrameShape(QFrame::StyledPanel);
    m_pane->setFrameShadow(QFrame::Sunken);
    auto *paneLayout = new QVBoxLayout(m_pane);
    paneLayout->setContentsMargins(2, 2, 2, 2);
    paneLayout->setSpacing(0);
    rightLayout->addWidget(m_pane, 1);

    m_views = new QStackedWidget(m_pane);
    // The two views are in scroll areas, so that the window can be made small: what it keeps is room for the waveform.
    m_combinedScroll = new QScrollArea(m_views);
    m_combinedScroll->setWidgetResizable(true);
    m_combinedScroll->setFrameShape(QFrame::NoFrame);
    m_combinedScroll->setWidget(buildCombined());
    m_combinedScroll->setMinimumSize(kViewMinimumWidth, kViewMinimumHeight);
    m_views->addWidget(m_combinedScroll);
    m_laneScroll = new QScrollArea(m_views);
    m_laneScroll->setWidgetResizable(true);
    m_laneScroll->setFrameShape(QFrame::NoFrame);
    m_laneScroll->setMinimumSize(kViewMinimumWidth, kViewMinimumHeight);
    m_laneHost = new QWidget(m_laneScroll);
    m_laneLayout = new QVBoxLayout(m_laneHost);
    m_laneLayout->setContentsMargins(0, 0, 0, 0);
    m_laneLayout->setSpacing(0);
    m_laneScroll->setWidget(m_laneHost);
    m_views->addWidget(m_laneScroll);
    paneLayout->addWidget(m_views, 1);

    // The navigation bar: it moves the bar in the waveform.
    auto *nav = new QWidget(m_pane);
    auto *navLayout = new QHBoxLayout(nav);
    navLayout->setContentsMargins(8, 6, 8, 2);
    auto *toStart = iconButton(QStringLiteral("start-frame.png"), tr("Start of the capture"), nav);
    auto *previous = iconButton(QStringLiteral("prev-frame.png"), tr("Previous window"), nav, true);
    auto *next = iconButton(QStringLiteral("next-frame.png"), tr("Next window"), nav, true);
    auto *toEnd = iconButton(QStringLiteral("end-frame.png"), tr("End of the capture"), nav);
    m_slider = new QSlider(Qt::Horizontal, nav);
    m_slider->setObjectName(QStringLiteral("navigationSlider"));
    m_slider->setRange(0, kSliderSteps);
    m_slider->setToolTip(tr("Moves the bar inside the window of the waveform"));
    m_position = new QLabel(nav);
    m_position->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_position->setMinimumWidth(130);
    m_position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    navLayout->addWidget(toStart);
    navLayout->addWidget(previous);
    navLayout->addWidget(m_slider, 1);
    navLayout->addWidget(next);
    navLayout->addWidget(toEnd);
    // The position (time of the bar, window n of N) is in the tip of the slider, which can go to the edge.
    m_position->hide();
    paneLayout->addWidget(nav);

    split->addWidget(left);
    split->addWidget(m_panel);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 0);
    split->setStretchFactor(2, 1);
    split->setSizes({kTrackListWidth, kMetadataWidth, 900});
    // The list of tracks can be put away: a strip with an arrow stays on the left to bring it back.
    auto *page = new QWidget(m_pages);
    auto *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    m_listToggle = new QToolButton(page);
    m_listToggle->setAutoRaise(false);   // with its outline, as the buttons of the navigation bar
    // A mini arrow that is beside the title of the first panel shown: Tracks, else Source, else the tabs.
    m_listToggle->setFixedSize(18, 18);
    m_listToggle->setObjectName(QStringLiteral("listToggle"));
    connect(m_listToggle, &QToolButton::clicked, this, [this] {
        resizeWithColumn(m_listColumn, &m_listWidth, !m_listColumn->isVisible(), [this](bool on) { setListShown(on); });
    });    pageLayout->addWidget(split, 1);
    m_pages->addWidget(page);
    applyPanels();
    setListShown(true);
    placeZoomBar();

    buildMenus();
    m_statusTracks = new QLabel(this);
    m_statusDuration = new QLabel(this);
    m_statusFile = new QLabel(this);
    statusBar()->addWidget(m_statusTracks);
    statusBar()->addWidget(m_statusDuration);
    statusBar()->addWidget(m_statusFile);
    // While a speed is worked out from the RF (it takes minutes): which track, and how far.
    m_statusSpeed = new QLabel(this);
    m_statusSpeed->setObjectName(QStringLiteral("speedProgress"));
    statusBar()->addPermanentWidget(m_statusSpeed);
    const auto showSpeedProgress = [this] {
        const int working = m_session->rfWorkingTracks();
        m_statusSpeed->setVisible(working > 0);
        m_statusSpeed->setText(working > 0 ? tr("Working out the TBC curve: %n track(s), %1%", nullptr, working).arg(m_session->rfMeanPercent()) : QString());
    };
    connect(m_session, &Session::rfSpeedChanged, this, showSpeedProgress);
    // (queued: the box is asked once the work that made it is over)
    connect(m_session, &Session::standardsDoubtful, this, &MainWindow::fixStandards, Qt::QueuedConnection);
    connect(m_session, &Session::rfSpeedProgress, this, showSpeedProgress);
    m_statusSpeed->hide();
    for (auto signal : {&Session::opened, &Session::closed, &Session::changed, &Session::rfSpeedChanged}) {
        connect(m_session, signal, this, [this] { updateVerify(); });
    }
    updateVerify();

    connect(m_tracks, &TrackList::rowClicked, this, [this](int row, bool wasSelected) {
        if (wasSelected) {
            showMetadata(!m_panelShown, true);
        } else {
            m_session->setSelected(row);
            showMetadata(true, true);
        }
    });
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int) { updateViews(); });
    connect(m_modeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_allLanes = id == 1;
        buildLanes();
    });
    // The buttons change the amplitude (the gain) of the graph, not the time: the wheel does that.
    connect(zoomOut, &QToolButton::clicked, this, [this] { m_amplitude->setValue(m_amplitude->value() / kAmplitudeStep); });
    connect(zoomIn, &QToolButton::clicked, this, [this] { m_amplitude->setValue(m_amplitude->value() * kAmplitudeStep); });
    connect(zoomFit, &QToolButton::clicked, this, [this] {
        {
            const QSignalBlocker block(m_amplitude);
            m_amplitude->setValue(1.0);
        }
        m_wave->resetGain();
        for (WaveformView *w : m_laneHost->findChildren<WaveformView *>()) {
            w->resetGain();
        }
        m_session->resetZoom();
    });
    connect(m_amplitude, &QDoubleSpinBox::valueChanged, this, &MainWindow::setAmplitude);
    connect(toStart, &QToolButton::clicked, this, [this] { m_session->goToStart(); });
    connect(previous, &QToolButton::clicked, this, [this] { m_session->pageBy(-m_session->windowLength()); });
    connect(next, &QToolButton::clicked, this, [this] { m_session->pageBy(m_session->windowLength()); });
    connect(toEnd, &QToolButton::clicked, this, [this] { m_session->goToEnd(); });
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if (!m_updating) {
            m_session->setCursor(m_session->windowStart() + static_cast<qint64>(static_cast<double>(value) / kSliderSteps * static_cast<double>(m_session->windowLength())));
        }
    });

    connect(m_session, &Session::opened, this, [this] {
        m_pages->setCurrentIndex(1);
        // One track alone: the list says nothing, so only what is known of it stays.
        setListShown(m_session->trackCount() > 1);
        if (m_session->trackCount() <= 1) {
            showMetadata(true);
        }
        // A file opened in the compact mode starts without the list of tracks and the page of metadata (the arrow and the
        // Info button bring them back).
        if (m_compact) {
            setListShown(false);
            showMetadata(false);
        }
        m_exportAction->setEnabled(true);
        m_closeAction->setEnabled(true);
        updateTitle();
        updateStatus();
        buildLanes();
    });
    connect(m_session, &Session::closed, this, [this] {
        m_openSources.clear();
        m_pages->setCurrentIndex(0);
        m_exportAction->setEnabled(false);
        m_closeAction->setEnabled(false);
        updateTitle();
        updateStatus();
    });
    connect(m_session, &Session::changed, this, [this] {
        updateNavigation();
        if (m_views->currentIndex() == kTabIndividual) {
            // Which tracks are shown, or which one is selected, makes other lanes.
            QString signature = QString::number(m_session->selected()) + QLatin1Char(':');
            for (int k = 0; k < m_session->trackCount(); ++k) {
                signature += m_session->visible(k) ? QLatin1Char('1') : QLatin1Char('0');
            }
            if (signature != m_laneSignature) {
                buildLanes();
            }
        }
    });

    showMetadata(true);
    updateRecent();
    updateViews();
    m_exportAction->setEnabled(false);
    m_closeAction->setEnabled(false);
    updateTitle();
    updateStatus();
    // The program opens in the compact mode when it was left in it: a preset, which the menu and the arrows can undo one
    // part at a time.
    if (m_compact) {
        setCompact(true);
    }
}

MainWindow::~MainWindow()
{
    // The session is a child, deleted after the members of this window: closed while
    // they are all still there, its signal does not reach a window half gone (the
    // compressed capture is read by it).
    m_session->close();
}

// The box "Align capture" of a waveform: the time base of its tracks corrected (from the TBC curve, the one the graph of the speed is
// made from), the tracks shown when `tracks` is empty. It stays on for the tracks that are shown afterwards (a track ticked in the list is
// aligned too). Without the lines of a track, it asks whether to work them out, for the tracks wanted only.
QCheckBox *MainWindow::alignBox(QWidget *parent)
{
    auto *box = new QCheckBox(tr("Align capture"), parent);
    box->setObjectName(QStringLiteral("alignBox"));
    box->setToolTip(tr("Correct the time base of the capture, as before stacking: every line at its place at the nominal speed, the same place in every capture. The waveform, the spectrum and the statistics are those of the resampled signal. It needs the TBC curve of the tracks (the one of the graph of the speed); without it, it proposes to work it out for the tracks shown."));
    const auto wanted = [this] {
        QVector<int> out;
        for (int k = 0; k < m_session->trackCount(); ++k) {
            if (m_session->visible(k)) {
                out.push_back(k);
            }
        }
        return out;
    };
    // `mode`: the box is on, for the tracks wanted now and for those that come to be.
    auto mode = std::make_shared<bool>(false);
    auto asking = std::make_shared<bool>(false);
    const auto sync = [this, box, wanted, mode] {
        const QVector<int> list = wanted();
        const QSignalBlocker block(box);
        box->setChecked(*mode && !list.isEmpty());
    };
    // Align the tracks wanted that are not: after asking when some have no lines yet (False: not wanted, the box goes off).
    const auto apply = [this, box, wanted, mode, asking, sync] {
        const QVector<int> list = wanted();
        QVector<int> todo;
        QStringList missing;
        for (int k : list) {
            if (!m_session->aligned(k)) {
                todo.push_back(k);
                if (!m_session->curveReady(k) && !m_session->rfSpeed(k)) {
                    missing << QString::number(k);
                }
            }
        }
        if (todo.isEmpty()) {
            return;
        }
        if (!missing.isEmpty()) {
            *asking = true;
            QString text = tr("The TBC curve of the track(s) %1 is not worked out yet. Work it out from the RF now? It takes a while (minutes for a large capture); the waveform is aligned when it is done.").arg(missing.join(QStringLiteral(", ")));
            if (!todo.contains(0) && !m_session->curveReady(0) && !m_session->rfSpeed(0)) {
                text += QLatin1Char('\n') + tr("The first track is worked out too: it gives the axis the others are lined up on.");
            }
            const auto answer = QMessageBox::question(this, tr("Align capture"), text);
            *asking = false;
            if (answer != QMessageBox::Yes) {
                *mode = false;
                sync();
                return;
            }
        }
        // The selected track first, so that it is the first to be done.
        for (int k : todo) {
            if (k == m_session->selected()) {
                m_session->setAligned(k, true);
            }
        }
        for (int k : todo) {
            if (k != m_session->selected()) {
                m_session->setAligned(k, true);
            }
        }
    };
    connect(m_session, &Session::rfSpeedChanged, box, sync);
    connect(m_session, &Session::opened, box, [mode, sync] {
        *mode = false;
        sync();
    });
    // A track shown while the box is on is aligned too (and asked for, when it has no lines yet).
    connect(m_session, &Session::changed, box, [mode, asking, apply, sync] {
        sync();
        if (*mode && !*asking) {
            apply();
        }
    });
    connect(box, &QCheckBox::toggled, this, [this, wanted, mode, apply, sync](bool on) {
        *mode = on;
        if (on) {
            apply();
            sync();
        } else {
            for (int k : wanted()) {
                m_session->setAligned(k, false);
            }
        }
    });
    {
        // (a lane made again starts as the state of the tracks is)
        const QVector<int> list = wanted();
        *mode = !list.isEmpty() && std::all_of(list.begin(), list.end(), [this](int k) { return m_session->aligned(k); });
    }
    sync();
    return box;
}
QWidget *MainWindow::buildCombined()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(4);

    auto *waveHead = heading(tr("RF waveform"), page);
    auto *hint = new QLabel(tr("click: move the bar · drag: select a zone · wheel: scroll · ctrl+wheel: zoom · shift+drag: pan"), waveHead);
    hint->setForegroundRole(QPalette::PlaceholderText);
    // The hint gives way (it is cut) when the room is short, rather than make the window wider.
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    hint->setMinimumWidth(0);
    static_cast<QHBoxLayout *>(waveHead->layout())->addStretch();
    waveHead->layout()->addWidget(hint);
    m_waveHeadLayout = static_cast<QHBoxLayout *>(waveHead->layout());
    m_waveHeadLayout->addSpacing(12);
    m_wave = new WaveformView(m_session, page);
    m_wave->setObjectName(QStringLiteral("combinedWaveform"));
    m_wave->setDrivesWindow(true);
    connect(m_wave, &WaveformView::gainChanged, this, [this](double gain) { showGain(gain); });
    m_wave->setFollowSize(m_followSize);
    layout->addWidget(waveHead);
    m_panelSeparator[kPanelWave] = makeSeparator(page);
    layout->addWidget(m_panelSeparator[kPanelWave]);
    auto *waveFrame = sunken(m_wave, page);
    layout->addWidget(waveFrame, 3);
    m_waveHeadLayout->insertWidget(1, makeCollapseButton(waveHead, [this](bool reduced) {
        panelReducing(kPanelWave, reduced, m_panelBody[kPanelWave]->height());
        m_waveReduced = reduced;
        applyPanels();
    }));
    m_panelHead[kPanelWave] = waveHead;
    m_panelBody[kPanelWave] = waveFrame;

    auto *spectrumHead = heading(tr("Spectrum"), page, &m_spectrumWhere);
    auto *row = static_cast<QHBoxLayout *>(spectrumHead->layout());
    // The controls of the spectrum are one widget, which goes where the spectrum is shown: in this heading, or in that of
    // the lane. The size of the FFT, the band, and the zoom as the waveform has it: the amplitude (gain) of the level
    // axis, with its buttons (the wheel zooms in frequency, and a drag moves the view).
    m_fftBar = new QWidget(spectrumHead);
    auto *fftLayout = new QHBoxLayout(m_fftBar);
    fftLayout->setContentsMargins(0, 0, 0, 0);
    fftLayout->setSpacing(4);
    fftLayout->addSpacing(12);
    fftLayout->addWidget(new QLabel(tr("FFT"), m_fftBar));
    m_fft = new QComboBox(m_fftBar);
    for (const int s : kFftSizes) {
        m_fft->addItem(fmt::count(s), s);
    }
    m_fft->setCurrentIndex(1);
    fftLayout->addWidget(m_fft);
    m_bands = new QCheckBox(tr("Band"), m_fftBar);
    m_bands->setChecked(true);
    m_bands->setToolTip(tr("Show only the part of the spectrum with the LaserDisc frequencies (the band the statistics are made over)"));
    fftLayout->addWidget(m_bands);
    fftLayout->addSpacing(8);
    fftLayout->addWidget(new QLabel(tr("Amplitude"), m_fftBar));
    m_fftGain = new QDoubleSpinBox(m_fftBar);
    m_fftGain->setRange(0.05, 256.0);
    m_fftGain->setDecimals(2);
    m_fftGain->setSingleStep(0.25);
    m_fftGain->setPrefix(QStringLiteral("x"));
    m_fftGain->setValue(1.0);
    m_fftGain->setKeyboardTracking(false);
    m_fftGain->setFixedWidth(84);
    m_fftGain->setObjectName(QStringLiteral("fftAmplitude"));
    m_fftGain->setToolTip(tr("The amplitude (the gain) of the spectrum. With the view normalised it shows what the normalisation comes to; changing it puts the normalisation off."));
    fftLayout->addWidget(m_fftGain);
    const auto fftButton = [this](const QString &icon, const QString &tip, bool repeat) {
        auto *b = iconButton(icon, tip, m_fftBar, repeat);
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        return b;
    };
    auto *fftOut = fftButton(QStringLiteral("zoom-out.png"), tr("Lower the amplitude of the spectrum; the wheel zooms in frequency, a drag moves the view"), true);
    auto *fftIn = fftButton(QStringLiteral("zoom-in.png"), tr("Raise the amplitude of the spectrum; the wheel zooms in frequency, a drag moves the view"), true);
    auto *fftFit = fftButton(QStringLiteral("zoom-original.png"), tr("Back to the default: the whole frequency range, and the amplitude 1"), false);
    fftLayout->addWidget(fftOut);
    fftLayout->addWidget(fftIn);
    fftLayout->addWidget(fftFit);
    row->addWidget(m_fftBar);
    connect(fftOut, &QToolButton::clicked, this, [this] { m_fftGain->setValue(m_fftGain->value() / kAmplitudeStep); });
    connect(fftIn, &QToolButton::clicked, this, [this] { m_fftGain->setValue(m_fftGain->value() * kAmplitudeStep); });
    connect(fftFit, &QToolButton::clicked, this, [this] {
        {
            const QSignalBlocker block(m_fftGain);
            m_fftGain->setValue(1.0);
        }
        m_spectrum->resetView();
        for (SpectrumView *s : m_laneHost->findChildren<SpectrumView *>()) {
            s->resetView();
        }
    });
    connect(m_fftGain, &QDoubleSpinBox::valueChanged, this, [this](double gain) {
        // Setting the amplitude by hand is not normalising.
        if (m_normaliseFft) {
            setNormaliseFft(false);
        }
        m_spectrum->setGain(gain);
        for (SpectrumView *s : m_laneHost->findChildren<SpectrumView *>()) {
            s->setGain(gain);
        }
    });
    m_spectrum = new SpectrumView(m_session, page);
    m_spectrum->setObjectName(QStringLiteral("combinedSpectrum"));
    m_spectrum->setNormalised(m_normaliseFft);
    connect(m_spectrum, &SpectrumView::gainChanged, this, [this](double gain) { showFftGain(gain); });
    layout->addWidget(spectrumHead);
    m_panelSeparator[kPanelSpectrum] = makeSeparator(page);
    layout->addWidget(m_panelSeparator[kPanelSpectrum]);
    auto *spectrumFrame = sunken(m_spectrum, page);
    layout->addWidget(spectrumFrame, 3);
    row->insertWidget(1, makeCollapseButton(spectrumHead, [this](bool reduced) {
        panelReducing(kPanelSpectrum, reduced, m_panelBody[kPanelSpectrum]->height());
        m_spectrumReduced = reduced;
        applyPanels();
    }));
    m_spectrumHeadLayout = row;
    m_panelHead[kPanelSpectrum] = spectrumHead;
    m_panelBody[kPanelSpectrum] = spectrumFrame;

    m_deviation = new DeviationBlock(m_session, page);
    m_deviation->setObjectName(QStringLiteral("combinedDeviation"));
    m_panelBody[kPanelDeviation] = m_deviation;
    connect(m_deviation, &DeviationBlock::reductionChanging, this, [this](bool reduced, int height) { panelReducing(kPanelDeviation, reduced, height); });
    layout->addWidget(m_deviation, 2);

    m_stats = new StatsPanel(m_session, page);
    m_stats->setObjectName(QStringLiteral("combinedStats"));
    m_panelBody[kPanelStats] = m_stats;
    connect(m_stats, &StatsPanel::reductionChanging, this, [this](bool reduced, int height) { panelReducing(kPanelStats, reduced, height); });
    layout->addWidget(m_stats, 1);
    // What is left when blocks are reduced or taken away is empty room under them: they stack at the top.
    layout->addStretch(0);

    connect(m_fft, &QComboBox::currentIndexChanged, this, [this](int) { m_session->setFftSize(m_fft->currentData().toInt()); });
    connect(m_bands, &QCheckBox::toggled, this, [this](bool on) {
        m_spectrum->setShowBand(on);
        for (SpectrumView *s : m_laneHost->findChildren<SpectrumView *>()) {
            s->setShowBand(on);
        }
    });
    connect(m_session, &Session::changed, this, [this] {
        const Selection s = m_session->selection();
        m_spectrumWhere->setText(s.range ? tr("zone %1 · %2 samples").arg(fmt::milliseconds(m_session->seconds(s.center)), fmt::count(s.length))
                                         : tr("bar %1 · %2 samples").arg(fmt::milliseconds(m_session->seconds(s.center)), fmt::count(s.length)));
    });
    return page;
}

void MainWindow::buildLanes()
{
    // The zoom bar is in a lane that goes: keep it.
    m_zoomBar->hide();
    m_zoomBar->setParent(this);
    m_fftBar->hide();
    m_fftBar->setParent(this);
    while (QLayoutItem *item = m_laneLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            // Gone from the screen at once, deleted when the event loop is back.
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
    if (!m_session->isOpen()) {
        return;
    }
    m_laneZoomSlot = nullptr;
    m_laneFftSlot = nullptr;
    m_laneSignature = QString::number(m_session->selected()) + QLatin1Char(':');
    for (int k = 0; k < m_session->trackCount(); ++k) {
        m_laneSignature += m_session->visible(k) ? QLatin1Char('1') : QLatin1Char('0');
    }
    QVector<int> tracks;
    if (m_allLanes) {
        for (int k = 0; k < m_session->trackCount(); ++k) {
            if (m_session->visible(k)) {
                tracks.push_back(k);
            }
        }
    } else {
        tracks.push_back(m_session->selected());
    }
    const double amp = m_amplitude->value();
    // All the lanes side by side: the controls of the two graphs have a row of their own above them, so that no lane has
    // wider graphs than another because of them.
    if (m_allLanes && !tracks.isEmpty()) {
        auto *tools = new QWidget(m_laneHost);
        auto *toolsRow = new QHBoxLayout(tools);
        toolsRow->setContentsMargins(10, 4, 10, 0);
        toolsRow->addStretch();
        m_laneZoomSlot = toolsRow;
        m_laneFftSlot = toolsRow;
        m_laneLayout->addWidget(tools);
    }
    for (int k : tracks) {
        const rfx::SourceMeta &sm = m_session->track(k);
        const double rate = sm.rateHz > 0 ? sm.rateHz : m_session->rateHz();
        auto *lane = new QWidget(m_laneHost);
        auto *layout = new QVBoxLayout(lane);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(6);

        auto *head = new QWidget(lane);
        auto *headRow = new QHBoxLayout(head);
        headRow->setContentsMargins(2, 0, 2, 0);
        auto *swatch = new QLabel(QStringLiteral("<span style=\"color:%1\">&#9632;</span>").arg(m_session->color(k).name()), head);
        auto *name = new QLabel(QString::number(k) + QLatin1Char(' ') + QString::fromStdString(sm.name), head);
        QFont bold = name->font();
        bold.setBold(true);
        name->setFont(bold);
        auto *info = new QLabel(fmt::duration(static_cast<double>(sm.words) / rate) + QStringLiteral(" · ") + fmt::bytes(m_session->storedBytes(k)) + (m_session->inArchive(k) ? tr(" stored") : QString()), head);
        info->setForegroundRole(QPalette::PlaceholderText);
        headRow->addWidget(swatch);
        headRow->addWidget(name);
        QToolButton *waveToggle = makeCollapseButton(head, nullptr);
        headRow->addWidget(waveToggle);
        headRow->addStretch();
        headRow->addWidget(info);
        if (k == tracks.first() && !m_allLanes) {
            m_laneZoomSlot = headRow;
        }
        layout->addWidget(head);

        auto *views = new QWidget(lane);
        QBoxLayout *box = m_allLanes ? static_cast<QBoxLayout *>(new QHBoxLayout(views)) : static_cast<QBoxLayout *>(new QVBoxLayout(views));
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(6);
        auto *wave = new WaveformView(m_session, views);
        wave->setTracks({k});
        wave->setAmplitude(amp);
        wave->setFollowSize(m_followSize);
        wave->setDrivesWindow(!m_allLanes);
        if (!m_allLanes) {
            connect(wave, &WaveformView::gainChanged, this, [this](double gain) { showGain(gain); });
        }
        auto *spectrum = new SpectrumView(m_session, views);
        spectrum->setTracks({k});
        spectrum->setShowBand(m_bands->isChecked());
        spectrum->setNormalised(m_normaliseFft);
        spectrum->setGain(m_fftGain->value());
        // The amplitude box shows what the normalisation comes to for the first lane, as it does for the combined view.
        if (k == tracks.first()) {
            connect(spectrum, &SpectrumView::gainChanged, this, [this](double gain) { showFftGain(gain); });
        }
        wave->setMinimumHeight(m_allLanes ? 90 : 100);
        spectrum->setMinimumHeight(m_allLanes ? 90 : 100);
        auto *waveBox = sunken(wave, views);
        connect(waveToggle, &QToolButton::toggled, waveBox, [waveBox](bool reduced) { waveBox->setVisible(!reduced); });
        auto *spectrumBox = sunken(spectrum, views);
        // The waveform and the spectrum each in a column, the spectrum with its heading (the amplitude of the spectrum and
        // its buttons are in the heading of the first lane).
        auto *waveCol = new QWidget(views);
        auto *waveColLayout = new QVBoxLayout(waveCol);
        waveColLayout->setContentsMargins(0, 0, 0, 0);
        waveColLayout->setSpacing(4);
        auto *specCol = new QWidget(views);
        auto *specColLayout = new QVBoxLayout(specCol);
        specColLayout->setContentsMargins(0, 0, 0, 0);
        specColLayout->setSpacing(4);
        auto *specHead = new QWidget(specCol);
        auto *specRow = new QHBoxLayout(specHead);
        specRow->setContentsMargins(2, 0, 2, 0);
        auto *specTitle = new QLabel(tr("Spectrum"), specHead);
        specTitle->setForegroundRole(QPalette::PlaceholderText);
        specRow->addWidget(specTitle);
        // (the arrows of the titles, as in the combined view: the graph is reduced to its title)
        specRow->addWidget(makeCollapseButton(specHead, [spectrumBox](bool reduced) { spectrumBox->setVisible(!reduced); }));
        specRow->addStretch();
        if (k == tracks.first() && !m_allLanes) {
            m_laneFftSlot = specRow;
        }
        if (m_allLanes) {
            // (side by side, the two have a title so that their graphs line up)
            auto *waveTitle = new QLabel(tr("RF waveform"), waveCol);
            waveTitle->setForegroundRole(QPalette::PlaceholderText);
            waveTitle->setFixedHeight(specHead->sizeHint().height());   // (as tall as the title of the spectrum, so that the graphs line up)
            waveColLayout->addWidget(waveTitle);
        }
        waveColLayout->addWidget(waveBox, 1);
        specColLayout->addWidget(specHead);
        specColLayout->addWidget(spectrumBox, 1);
        waveCol->setVisible(m_showPanel[kPanelWave]);
        specCol->setVisible(m_showPanel[kPanelSpectrum]);
        views->setVisible(m_showPanel[kPanelWave] || m_showPanel[kPanelSpectrum]);
        box->addWidget(waveCol, 1);
        box->addWidget(specCol, 1);
        // One track alone fills the window: the graphs share the room, the statistics take what they need and more.
        layout->addWidget(views, m_allLanes ? 0 : 3);

        auto *deviation = new DeviationBlock(m_session, lane);
        deviation->setTracks({k});
        deviation->setExpandedMinimum(m_allLanes ? 70 : 80);
        deviation->setVisible(m_showPanel[kPanelDeviation]);
        layout->addWidget(deviation);
        auto *stats = new StatsPanel(m_session, lane);
        stats->setTracks({k});
        stats->setVisible(m_showPanel[kPanelStats]);
        layout->addWidget(stats, m_allLanes ? 0 : 1);
        m_laneLayout->addWidget(lane, m_allLanes ? 0 : 1);
    }
    if (m_allLanes) {
        m_laneLayout->addStretch(1);
    }
    placeZoomBar();
}

// The zoom bar goes in the heading of the waveform that is shown: that of the combined view, or of the first lane.
// A block of the views put away or brought back (by the menu, or by the compact mode, which does not keep it).
void MainWindow::setPanelVisible(int panel, bool on, bool keep)
{
    static const char *const keys[] = {"panelWave", "panelSpectrum", "panelDeviation", "panelStats"};
    if (m_showPanel[panel] == on) {
        return;
    }
    // The window gets the room of the block back, or gives it.
    if (!on) {
        m_shownHeight[panel] = panelExtent(panel);
    }
    m_showPanel[panel] = on;
    if (keep) {
        QSettings().setValue(QLatin1String(keys[panel]), on);
    }
    if (m_panelActions[panel] && m_panelActions[panel]->isChecked() != on) {
        const QSignalBlocker block(m_panelActions[panel]);
        m_panelActions[panel]->setChecked(on);
    }
    applyPanels();
    if (m_tabs->currentIndex() == kTabIndividual) {
        buildLanes();
    }
    placeZoomBar();
    resizeBy(on ? m_shownHeight[panel] : -m_shownHeight[panel]);
}

void MainWindow::setCompact(bool on)
{
    QSettings().setValue(QStringLiteral("compactMode"), on);
    if (m_compactAction && m_compactAction->isChecked() != on) {
        const QSignalBlocker block(m_compactAction);
        m_compactAction->setChecked(on);
    }
    if (m_compactApplied == on) {
        return;
    }
    m_compact = on;
    m_compactApplied = on;
    if (on) {
        for (int p = 0; p < kPanelCount; ++p) {
            m_beforeCompact[p] = m_showPanel[p];
        }
        m_listBeforeCompact = m_listColumn->isVisible();
        m_infoBeforeCompact = m_panelShown;
        setPanelVisible(kPanelWave, true, false);
        setPanelVisible(kPanelSpectrum, true, false);
        setPanelVisible(kPanelDeviation, false, false);
        setPanelVisible(kPanelStats, false, false);
        setListShown(false);
        showMetadata(false);
        if (!isMaximized() && !isFullScreen()) {
            resize(std::min(width(), kCompactWidth), std::min(height(), kCompactHeight));
        }
    } else {
        for (int p = 0; p < kPanelCount; ++p) {
            setPanelVisible(p, m_beforeCompact[p], false);
        }
        setListShown(m_listBeforeCompact);
        showMetadata(m_infoBeforeCompact);
    }
}

// The window follows what is reduced or put away: as tall as the block was when it goes, and as tall again when it
// comes back (not when the window is maximised, where there is nothing to follow).
void MainWindow::resizeBy(int dy)
{
    if (dy == 0 || isMaximized() || isFullScreen() || m_pages->currentIndex() != 1) {
        return;
    }
    QTimer::singleShot(0, this, [this, dy] {
        resize(width(), std::max(minimumSizeHint().height(), height() + dy));
    });
}

// The same across: the window as wide as the list of tracks it shows or puts away (not when it is maximised).
void MainWindow::resizeWidthBy(int dx)
{
    if (dx == 0 || isMaximized() || isFullScreen() || m_pages->currentIndex() != 1) {
        return;
    }
    QTimer::singleShot(0, this, [this, dx] {
        resize(std::max(minimumSizeHint().width(), width() + dx), height());
    });
}

// A column at the side that comes or goes by the choice of the user, and the window with it: as wide as the column was when it goes, and as wide
// again when it comes back (what it takes once it is laid out, the splitter handle with it). `change` shows or hides it.
void MainWindow::resizeWithColumn(QWidget *column, int *remembered, bool show, const std::function<void(bool)> &change)
{
    const auto *splitter = qobject_cast<QSplitter *>(column->parentWidget());
    const int handle = splitter ? splitter->handleWidth() : 0;
    if (!show) {
        *remembered = column->width() + handle;
        change(false);
        resizeWidthBy(-*remembered);
        return;
    }
    change(true);
    QTimer::singleShot(0, this, [this, column, remembered, handle] {
        *remembered = column->width() + handle;
        if (!isMaximized() && !isFullScreen() && m_pages->currentIndex() == 1) {
            resize(std::max(minimumSizeHint().width(), width() + *remembered), height());
        }
    });
}

void MainWindow::panelReducing(int panel, bool reduced, int bodyHeight)
{
    constexpr int kGap = 4;
    if (reduced && bodyHeight > 0) {
        m_shownHeight[panel] = bodyHeight + kGap;
    }
    resizeBy(reduced ? -m_shownHeight[panel] : m_shownHeight[panel]);
}

// How much of the window a block takes now: its title, its body, and the gaps.
int MainWindow::panelExtent(int panel) const
{
    constexpr int kGap = 4;
    int extent = 0;
    if (m_panelHead[panel] && m_panelHead[panel]->isVisible()) {
        extent += m_panelHead[panel]->height() + kGap;
    }
    if (m_panelBody[panel] && m_panelBody[panel]->isVisible()) {
        extent += m_panelBody[panel]->height() + kGap;
    }
    return std::max(extent, 40);
}

// The blocks of the combined view that are shown: the menu says which, the arrows in the titles which are reduced.
void MainWindow::applyPanels()
{
    const bool reduced[] = {m_waveReduced, m_spectrumReduced, false, false};
    for (int p = 0; p < kPanelCount; ++p) {
        if (!m_panelBody[p]) {
            continue;
        }
        if (m_panelHead[p]) {
            m_panelHead[p]->setVisible(m_showPanel[p]);
        }
        m_panelBody[p]->setVisible(m_showPanel[p] && !reduced[p]);
        if (m_panelSeparator[p]) {
            m_panelSeparator[p]->setVisible(m_showPanel[p] && reduced[p]);
        }
    }
}

// The controls of the spectrum go with the spectrum that is shown: in its heading in the combined view, in the one of the
// first lane in the individual one.
void MainWindow::placeFftBar()
{
    QHBoxLayout *slot = nullptr;
    if (m_showPanel[kPanelSpectrum]) {
        slot = m_tabs->currentIndex() == kTabIndividual ? m_laneFftSlot : m_spectrumHeadLayout;
    }
    if (!slot) {
        m_fftBar->hide();
        return;
    }
    slot->insertWidget(slot->count(), m_fftBar);
    m_fftBar->show();
}

void MainWindow::placeZoomBar()
{
    placeZoomBarOnly();
    placeFftBar();
}

void MainWindow::placeZoomBarOnly()
{
    QHBoxLayout *slot = m_laneZoomSlot;
    if (m_tabs->currentIndex() != kTabIndividual) {
        // The waveform and the spectrum share the window: the bar is with whichever is shown.
        slot = m_showPanel[kPanelWave] ? m_waveHeadLayout : (m_showPanel[kPanelSpectrum] ? m_spectrumHeadLayout : nullptr);
    }
    if (!slot) {
        m_zoomBar->hide();
        return;
    }
    slot->insertWidget(slot->count(), m_zoomBar);
    m_zoomBar->show();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    QAction *open = file->addAction(tr("&Open RF file…"), this, &MainWindow::chooseFile);
    open->setShortcut(QKeySequence::Open);
    QAction *openFolder = file->addAction(tr("Open &folder…"), this, &MainWindow::chooseFolder);
    openFolder->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    m_recentMenu = file->addMenu(tr("Open &recent"));
    file->addSeparator();
    m_exportAction = file->addAction(tr("&Export metadata as YAML…"), this, &MainWindow::exportYaml);
    m_verifyAction = file->addAction(tr("Chec&k the file"), this, [this] { verifyTracks(std::nullopt); });
    m_verifyAction->setObjectName(QStringLiteral("verifyFile"));
    m_verifyAction->setToolTip(tr("Decode every track of the .rfx files that are open and compare it with the SHA-256 the file keeps of its samples (a track alone: its Check button in the panel of the source)"));
    m_saveSpeedAction = file->addAction(tr("Save TBC &curve"), this, &MainWindow::saveSpeed);
    m_saveSpeedAction->setToolTip(tr("Keep the TBC curves that are worked out in the .rfx file (added at its end, nothing else in it changes; they are shown at once next time)."));
    connect(file, &QMenu::aboutToShow, this, [this] { m_saveSpeedAction->setEnabled(m_session->isOpen() && m_session->canSaveSpeed());
        updateVerify(); });
    m_closeAction = file->addAction(tr("&Close file"), this, &MainWindow::closeFile);
    m_closeAction->setShortcut(QKeySequence::Close);
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    m_metadataAction = view->addAction(tr("&Metadata panel"));
    m_metadataAction->setCheckable(true);
    m_metadataAction->setChecked(true);
    m_metadataAction->setShortcut(QKeySequence(Qt::Key_I));
    connect(m_metadataAction, &QAction::toggled, this, [this](bool on) {
        if (on != m_panelShown) {
            showMetadata(on, true);
        }
    });
    view->addAction(tr("&Reset zoom"), this, [this] { m_session->resetZoom(); });
    // On (as it starts), the gain of the waveform goes up when the graph is made taller, so that the signal fills the
    // same part of it. Off, the gain stays and the graph shows more of the signal.
    QAction *followSize = view->addAction(tr("Change gain on resize"));
    followSize->setCheckable(true);
    followSize->setChecked(m_followSize);
    followSize->setToolTip(tr("On: a taller waveform gets more gain, so that the signal fills the same part of it. Off: the gain stays and the waveform shows more of the signal (and more time, when it is wider)."));
    connect(followSize, &QAction::toggled, this, [this](bool on) {
        m_followSize = on;
        QSettings().setValue(QStringLiteral("gainOnResize"), on);
        m_wave->setFollowSize(on);
        for (WaveformView *w : m_laneHost->findChildren<WaveformView *>()) {
            w->setFollowSize(on);
        }
    });
    // The noise the stack removes (Noise residue): on (as it starts), one curve for all the tracks, the spread of the tracks round the stack;
    // off, a curve for each track, in its colour.
    QAction *combinedResidue = view->addAction(tr("Show combined residue"));
    combinedResidue->setObjectName(QStringLiteral("combinedResidue"));
    combinedResidue->setCheckable(true);
    combinedResidue->setChecked(QSettings().value(QStringLiteral("combinedResidue"), true).toBool());
    m_session->setCombinedResidue(combinedResidue->isChecked());
    combinedResidue->setToolTip(tr("On: the noise residue is one curve for all the tracks (their spread round the stack). Off: one curve for each track, in its colour."));
    connect(combinedResidue, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("combinedResidue"), on);
        m_session->setCombinedResidue(on);
    });
    // The spectrum can be normalised (the decibels it covers stay, the curve is stretched with the graph), or not: then
    // only the coordinates change, and a taller graph covers more decibels.
    QAction *normalise = view->addAction(tr("Normalise FFT view"));
    normalise->setCheckable(true);
    normalise->setChecked(m_normaliseFft);
    normalise->setToolTip(tr("On: the gain of the spectrum is worked out so that its highest peak is at 90% of the graph and the curve fills it (the amplitude shows what it comes to). Off: the amplitude is the one that is set, and a taller graph covers more decibels."));
    m_normaliseAction = normalise;
    connect(normalise, &QAction::toggled, this, [this](bool on) { setNormaliseFft(on); });
    // Each block of the views can be taken away altogether; all are shown to begin with.
    view->addSeparator();
    const struct {
        int panel;
        const char *name;
        const char *key;
    } panels[] = {{kPanelWave, QT_TR_NOOP("RF &waveform"), "panelWave"}, {kPanelSpectrum, QT_TR_NOOP("&Spectrum"), "panelSpectrum"},
                  {kPanelDeviation, QT_TR_NOOP("Speed &deviation"), "panelDeviation"}, {kPanelStats, QT_TR_NOOP("S&tatistics"), "panelStats"}};
    for (const auto &p : panels) {
        QAction *action = view->addAction(tr(p.name));
        action->setCheckable(true);
        action->setChecked(m_showPanel[p.panel]);
        m_panelActions[p.panel] = action;
        connect(action, &QAction::toggled, this, [this, p](bool on) { setPanelVisible(p.panel, on, true); });
    }
    // The compact mode: only the waveform and the spectrum, the other blocks away (as the menu says) and the lists put
    // away. It is kept: the program opens in it next time. Out of it, everything comes back as it was.
    view->addSeparator();
    m_compactAction = view->addAction(tr("Open in &compact mode"));
    m_compactAction->setCheckable(true);
    m_compactAction->setChecked(m_compact);
    m_compactAction->setToolTip(tr("Only the waveform and the spectrum, in a small window; the other blocks and the lists of tracks and metadata are put away. Kept for the next time."));
    connect(m_compactAction, &QAction::toggled, this, [this](bool on) { setCompact(on); });
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
    help->addAction(tr("&About RF-analyse"), this, [this] {
        QMessageBox::about(this, tr("About RF-analyse"),
                           tr("<b>RF-analyse</b> %1<br>Looks at the RF captures stored in an .rfx file: the waveform, the spectrum, the noise and the drift of each track.<br>Part of RF-tools, GPL-3.0-or-later.<br><br>Contributors: vrunk11.").arg(QCoreApplication::applicationVersion()));
    });
}

void MainWindow::showMetadata(bool shown, bool follow)
{
    if (follow && shown != m_panelShown) {
        resizeWithColumn(m_panel, &m_panelWidth, shown, [this](bool on) { showMetadata(on, false); });
        return;
    }
    m_panelShown = shown;
    placeListToggle();
    m_panel->setVisible(shown);
    m_tracks->setPanelShown(shown);
    m_metadataAction->blockSignals(true);
    m_metadataAction->setChecked(shown);
    m_metadataAction->blockSignals(false);
    if (m_infoButton) {
        const QSignalBlocker block(m_infoButton);
        m_infoButton->setChecked(shown);
    }
}

void MainWindow::updateViews()
{
    const bool individual = m_tabs->currentIndex() == kTabIndividual;
    m_views->setCurrentIndex(individual ? 1 : 0);
    // The page that is not shown does not count in the smallest size of the window.
    for (int i = 0; i < m_views->count(); ++i) {
        const bool current = i == m_views->currentIndex();
        m_views->widget(i)->setSizePolicy(current ? QSizePolicy::Preferred : QSizePolicy::Ignored, current ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    }
    m_modeBox->setVisible(individual);
    // (the stack and its residue are of the combined view)
    m_stackBox->setVisible(!individual);
    m_residueBox->setVisible(!individual);
    if (individual) {
        buildLanes();
    }
    placeZoomBar();
}

// The spectrum normalised or not: the amplitude of the spectrum comes from the normalisation, or is the number that is set.
void MainWindow::setNormaliseFft(bool on)
{
    m_normaliseFft = on;
    QSettings().setValue(QStringLiteral("normaliseFft"), on);
    if (m_normaliseAction && m_normaliseAction->isChecked() != on) {
        const QSignalBlocker block(m_normaliseAction);
        m_normaliseAction->setChecked(on);
    }
    m_spectrum->setNormalised(on);
    for (SpectrumView *s : m_laneHost->findChildren<SpectrumView *>()) {
        s->setNormalised(on);
    }
    if (!on) {
        // What the normalisation came to stays, as the gain that is set.
        m_spectrum->setGain(m_fftGain->value());
        for (SpectrumView *s : m_laneHost->findChildren<SpectrumView *>()) {
            s->setGain(m_fftGain->value());
        }
    }
}

void MainWindow::showFftGain(double gain)
{
    const QSignalBlocker block(m_fftGain);
    m_fftGain->setValue(std::clamp(gain, m_fftGain->minimum(), m_fftGain->maximum()));
}

// The number in the amplitude box says what the graph shows: when a resize puts more or less of the range on it, it changes.
void MainWindow::showGain(double gain)
{
    const QSignalBlocker block(m_amplitude);
    m_amplitude->setValue(std::clamp(gain, kAmplitudeLowest, kAmplitudeHighest));
}

void MainWindow::setAmplitude(double a)
{
    m_wave->setAmplitude(a);
    for (WaveformView *w : m_laneHost->findChildren<WaveformView *>()) {
        w->setAmplitude(a);
    }
}

void MainWindow::updateNavigation()
{
    if (!m_session->isOpen()) {
        return;
    }
    m_updating = true;
    const Selection s = m_session->selection();
    const double rel = static_cast<double>(s.center - m_session->windowStart()) / static_cast<double>(m_session->windowLength());
    m_slider->setValue(static_cast<int>(std::clamp(rel, 0.0, 1.0) * kSliderSteps));
    m_updating = false;
    const bool off = rel < 0.0 || rel > 1.0;
    const qint64 length = m_session->windowLength();
    const qint64 pages = (m_session->totalSamples() + length - 1) / length;
    m_position->setText(fmt::milliseconds(m_session->seconds(s.center)) + QLatin1Char('\n') + (off ? tr("off screen") : tr("window %1 of %2").arg(m_session->windowStart() / length + 1).arg(pages)));
    QString where = m_position->text();
    where.replace(QLatin1Char('\n'), QLatin1String(" · "));
    m_slider->setToolTip(tr("Moves the bar inside the window of the waveform") + QLatin1Char('\n') + where);
    m_zoomLabel->setText(tr("%1 samples · %2 µs").arg(fmt::count(length), QString::number(m_session->seconds(length) * 1e6, 'f', 1)));
}

void MainWindow::updateStatus()
{
    if (!m_session->isOpen()) {
        // Empty, they would show as three small blocks.
        for (QLabel *label : {m_statusTracks, m_statusDuration, m_statusFile}) {
            label->clear();
            label->setVisible(false);
        }
        return;
    }
    for (QLabel *label : {m_statusTracks, m_statusDuration, m_statusFile}) {
        label->setVisible(true);
    }
    const uint64_t size = m_session->fileSize();
    m_statusTracks->setText(tr("%n track(s)", nullptr, m_session->trackCount()));
    // The longest track sets the timeline; the others may be shorter, and end before it.
    qint64 shortest = m_session->totalSamples();
    for (int k = 0; k < m_session->trackCount(); ++k) {
        shortest = std::min<qint64>(shortest, static_cast<qint64>(m_session->track(k).words));
    }
    m_statusDuration->setText(tr("Duration %1 at %2%3%4")
                                  .arg(fmt::duration(m_session->seconds(m_session->totalSamples())), fmt::megahertz(m_session->rateHz()), m_session->rateKnown() ? QString() : tr(" (assumed)"),
                                       shortest < m_session->totalSamples() ? tr(" (the longest track; the shortest lasts %1)").arg(fmt::duration(m_session->seconds(shortest))) : QString()));
    if (m_session->sourceCount() > 1) {
        m_statusFile->setText(tr("%n file(s), %1", nullptr, m_session->sourceCount()).arg(fmt::bytes(size)));
        return;
    }
    // An archive is compared with the captures it holds; a capture is what it is.
    uint64_t originals = 0;
    for (int k = 0; k < m_session->trackCount(); ++k) {
        originals += m_session->track(k).size;
    }
    const QString name = QFileInfo(m_session->path()).fileName();
    m_statusFile->setText(m_session->hasArchive() ? tr("%1 %2 for %3 of captures (%4)").arg(name, fmt::bytes(size), fmt::bytes(originals), fmt::percent(size, originals))
                                                  : tr("%1 %2").arg(name, fmt::bytes(size)));
}

void MainWindow::updateTitle()
{
    if (!m_session->isOpen()) {
        setWindowTitle(tr("RF-analyse"));
        return;
    }
    const QString first = QFileInfo(m_session->path()).fileName();
    const int others = m_session->sourceCount() - 1;
    setWindowTitle(others > 0 ? tr("RF-analyse - %1 and %n other file(s)", nullptr, others).arg(first) : tr("RF-analyse - %1").arg(first));
}

void MainWindow::rememberRecent(const QStringList &paths)
{
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("recent")).toStringList();
    for (const QString &path : paths) {
        recent.removeAll(path);
    }
    for (int i = paths.size() - 1; i >= 0; --i) {
        recent.prepend(paths[i]);
    }
    while (recent.size() > kRecentFiles) {
        recent.removeLast();
    }
    settings.setValue(QStringLiteral("recent"), recent);
    updateRecent();
    m_amplitude->setValue(1.0);
}

bool MainWindow::openFile(const QString &path)
{
    return openFiles({path});
}

// The sources for files: an .rfx archive says what it holds. A compressed capture (.flac, .ldf) says its sample rate in
// its header (in kHz, for the captures of ld-decode), or the .yaml beside it does: it is not asked. Any other capture says
// nothing of its format: ask, once for all of them. False when that was cancelled.
bool MainWindow::sourcesFor(const QStringList &paths, std::vector<Session::Source> &sources)
{
    sources.clear();
    std::vector<std::optional<rfx::CaptureSpec>> known(static_cast<size_t>(paths.size()));
    QStringList ask;
    for (int i = 0; i < paths.size(); ++i) {
        const QString &path = paths[i];
        if (isArchiveFile(path)) {
            continue;
        }
        // The .yaml beside a capture that declares its format and its rate says all there is to ask.
        if (const std::optional<rfx::DeclaredCapture> declared = rfx::declaredCapture(rfx::findSidecars(path))) {
            if (declared->compressed == isCompressedCapture(path)) {
                rfx::CaptureSpec spec;
                spec.format = declared->format;
                spec.compressed = declared->compressed;
                spec.rateHz = declared->rateHz;
                spec.name = QFileInfo(path).fileName().toStdString();
                known[static_cast<size_t>(i)] = spec;
                continue;
            }
        }
        if (isCompressedCapture(path)) {
            double rate = rfx::sidecarRateHz(rfx::findSidecars(path)).value_or(0.0);
            if (rate <= 0.0) {
                if (const std::optional<rfx::CompressedHeader> header = rfx::probeCompressed(path)) {
                    // (the header of the FLAC holds the rate in kHz, which it can hold: 40000 for 40 MHz)
                    rate = header->headerRate > 0 ? (header->headerRate < 1000000 ? header->headerRate * 1000.0 : static_cast<double>(header->headerRate)) : 0.0;
                }
            }
            if (rate > 0.0) {
                rfx::CaptureSpec spec;
                spec.format = rfx::SampleFormat::S16;
                spec.compressed = true;
                spec.rateHz = rate;
                spec.name = QFileInfo(path).fileName().toStdString();
                known[static_cast<size_t>(i)] = spec;   // (the bits it carries are found from its first samples)
                continue;
            }
        }
        ask << path;
    }
    std::unique_ptr<CaptureDialog> dialog;
    if (!ask.isEmpty()) {
        dialog = std::make_unique<CaptureDialog>(ask, this);
        if (dialog->exec() != QDialog::Accepted) {
            return false;
        }
    }
    int row = 0;
    for (int i = 0; i < paths.size(); ++i) {
        Session::Source s;
        s.path = paths[i];
        if (!isArchiveFile(paths[i])) {
            s.capture = true;
            s.spec = known[static_cast<size_t>(i)] ? *known[static_cast<size_t>(i)] : dialog->spec(row++);
        }
        sources.push_back(s);
    }
    return true;
}

bool MainWindow::openFiles(const QStringList &paths)

{
    if (paths.isEmpty()) {
        return false;
    }
    rememberFolder(QFileInfo(paths.first()).absolutePath());
    for (const QString &path : paths) {
        if (!QFileInfo(path).isFile()) {
            QMessageBox::warning(this, tr("Cannot open the file"), tr("%1\n\nThere is no such file.").arg(path));
            return false;
        }
    }
    std::vector<Session::Source> sources;
    if (!sourcesFor(paths, sources)) {
        return false;
    }
    return openSources(sources);
}

bool MainWindow::openCapture(const QString &path, const rfx::CaptureSpec &spec)
{
    Session::Source s;
    s.path = path;
    s.capture = true;
    s.spec = spec;
    return openSources({s});
}

bool MainWindow::openSources(std::vector<Session::Source> sources, bool append)
{
    if (append) {
        sources.insert(sources.begin(), m_openSources.begin(), m_openSources.end());
    }
    // A compressed capture is not decoded to a file: its samples are decoded where they are looked at.
    QString error;
    if (!m_session->open(sources, &error)) {
        const QString first = sources.front().shownAs.isEmpty() ? sources.front().path : sources.front().shownAs;
        QMessageBox::warning(this, tr("Cannot open the file"), sources.size() == 1 ? tr("%1\n\n%2").arg(first, error) : error);
        return false;
    }
    m_openSources = sources;
    QStringList shown;
    for (const Session::Source &s : sources) {
        shown << (s.shownAs.isEmpty() ? s.path : s.shownAs);
    }
    rememberRecent(shown);
    return true;
}

void MainWindow::addFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open more RF files"), lastFolder(), captureFilters().join(QLatin1String(";;")));
    rememberFolder(paths.isEmpty() ? QString() : QFileInfo(paths.first()).absolutePath());
    addPaths(paths);
}

// The folder the person last opened files from, where the next dialog starts (kept between runs).
QString MainWindow::lastFolder() const
{
    const QString folder = QSettings().value(QStringLiteral("lastDir")).toString();
    return QFileInfo(folder).isDir() ? folder : QString();
}

void MainWindow::rememberFolder(const QString &folder)
{
    if (!folder.isEmpty()) {
        QSettings().setValue(QStringLiteral("lastDir"), folder);
    }
}

// Files to open: added to the ones that are open (a drop and the button both do this), else opened.
void MainWindow::addPaths(const QStringList &given)
{
    if (given.isEmpty()) {
        return;
    }
    rememberFolder(QFileInfo(given.first()).absolutePath());
    if (!m_session->isOpen()) {
        openFiles(given);
        return;
    }
    // A file that is open already is not opened twice.
    const QStringList paths = notOpenYet(given);
    if (paths.isEmpty()) {
        return;
    }
    std::vector<Session::Source> sources;
    if (!sourcesFor(paths, sources)) {
        return;
    }
    openSources(sources, true);
}

// Where the arrow of the list is: beside Tracks when the list is there, beside Source when only that panel is,
// and before the tabs (which move a little to make room) when both are put away.
void MainWindow::placeListToggle()
{
    if (!m_listToggle || !m_listColumn) {
        return;
    }
    const bool listShown = !m_listColumn->isHidden();
    if (listShown) {
        m_tracksHeadLayout->insertWidget(m_tracksHeadLayout->count(), m_listToggle);
    } else if (m_panelShown) {
        m_panelHeadLayout->insertWidget(0, m_listToggle);
    } else {
        m_tabLayout->insertWidget(0, m_listToggle);
    }
    m_listToggle->show();
}

void MainWindow::setListShown(bool shown)
{
    m_listColumn->setVisible(shown);
    m_listToggle->setText(shown ? QString(QChar(0x2039)) : QString(QChar(0x203A)));
    m_listToggle->setToolTip(shown ? tr("Hide the list of tracks") : tr("Show the list of tracks"));
    placeListToggle();
}

// The files of `paths` that are not open yet, each once; the others are said to be skipped.
QStringList MainWindow::notOpenYet(const QStringList &paths)
{
    const auto key = [](const QString &path) {
        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
#ifdef Q_OS_WIN
        return canonical.toLower();
#else
        return canonical;
#endif
    };
    QSet<QString> seen;
    for (const Session::Source &s : m_openSources) {
        seen.insert(key(s.shownAs.isEmpty() ? s.path : s.shownAs));
    }
    QStringList fresh;
    int skipped = 0;
    for (const QString &path : paths) {
        if (seen.contains(key(path))) {
            ++skipped;
            continue;
        }
        seen.insert(key(path));
        fresh << path;
    }
    if (skipped > 0) {
        statusBar()->showMessage(tr("%n file(s) already open, not added again", nullptr, skipped), 6000);
    }
    return fresh;
}

// Files from File > Open or a drop, with files already open: add them or replace those, as the user says.
void MainWindow::openOrAdd(const QStringList &given)
{
    if (given.isEmpty()) {
        return;
    }
    rememberFolder(QFileInfo(given.first()).absolutePath());
    if (!m_session->isOpen()) {
        openFiles(given);
        return;
    }
    // Nothing to ask when everything that is brought is open already.
    const QStringList paths = notOpenYet(given);
    if (paths.isEmpty()) {
        return;
    }
    OpenMode mode = m_openMode;
    if (mode == OpenMode::Ask) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Files are already open"));
        box.setText(tr("Some files are already open."));
        box.setInformativeText(tr("Add the %n new file(s) to them, or replace them?", nullptr, static_cast<int>(paths.size())));
        QPushButton *add = box.addButton(tr("Add"), QMessageBox::AcceptRole);
        QPushButton *replace = box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(add);
        box.exec();
        if (box.clickedButton() == add) {
            mode = OpenMode::Add;
        } else if (box.clickedButton() == replace) {
            mode = OpenMode::Replace;
        } else {
            return;
        }
    }
    if (mode == OpenMode::Add) {
        addPaths(paths);
    } else {
        // (each file once, even if it was brought twice)
        QStringList once;
        QSet<QString> seen;
        for (const QString &path : given) {
            const QString id = QFileInfo(path).canonicalFilePath().toLower();
            if (!seen.contains(id)) {
                seen.insert(id);
                once << path;
            }
        }
        openFiles(once);
    }
}

// File > Open folder: the RF files in it (not in the folders under it), as if they had been chosen one by one.
void MainWindow::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Open a folder of RF files"), lastFolder());
    if (folder.isEmpty()) {
        return;
    }
    rememberFolder(folder);
    openFolder(folder);
}

bool MainWindow::openFolder(const QString &folder)
{
    const QStringList files = rfFilesInFolder(folder);
    if (files.isEmpty()) {
        QMessageBox::information(this, tr("Nothing to open"), tr("%1\n\nThere is no RF file in this folder (.rfx, .s16 .r16 .s8 .r8 .lds, .flac .ldf).").arg(folder));
        return false;
    }
    openOrAdd(files);
    return true;
}

void MainWindow::chooseFile()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open RF files"), lastFolder(), captureFilters().join(QLatin1String(";;")));
    if (!paths.isEmpty()) {
        rememberFolder(QFileInfo(paths.first()).absolutePath());
        openOrAdd(paths);
    }
}

void MainWindow::editTrack()
{
    const int k = m_session->selected();
    if (!m_session->isOpen() || k >= m_session->trackCount()) {
        return;
    }
    if (!m_session->inArchive(k)) {
        QMessageBox::information(this, tr("Edit"), tr("A capture on its own keeps no metadata: nothing can be said of it. Pack it in an .rfx file (rf-packer) first."));
        return;
    }
    TrackEditDialog dialog(m_session->track(k), this);
    if (dialog.exec() != QDialog::Accepted || !dialog.changed()) {
        return;
    }
    rewriteTrack(k, dialog.edit());
}

bool MainWindow::rewriteTrack(int k, const rfx::TrackEdit &edit, bool quiet)
{
    if (!m_session->isOpen() || k < 0 || k >= m_session->trackCount() || !m_session->inArchive(k)) {
        return false;
    }
    return rewriteFile(m_session->sourcePath(k), {{m_session->localTrack(k), edit}}, k, quiet);
}

bool MainWindow::rewriteFile(const QString &path, const std::map<size_t, rfx::TrackEdit> &edits, int select, bool quiet)
{
    if (!m_session->isOpen()) {
        return false;
    }
    const auto say = [this, quiet](const QString &text) {
        if (!quiet) {
            QMessageBox::warning(this, tr("Edit"), text);
        }
    };
    // What is worked out and not in the file would be lost when it is read again: asked first.
    if (!confirmCurves()) {
        return false;
    }
    const std::vector<Session::Source> sources = m_openSources;
    // The files are let go (this one is replaced), and opened again after.
    m_session->close();
    const QFileInfo info(path);
    const QString aside = info.dir().filePath(info.completeBaseName() + QStringLiteral(".edit.rfx"));
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto error = std::make_shared<QString>();
    std::thread worker([path, aside, edits, done, error] {
        try {
            std::vector<rfx::Pick> picks = rfx::pickAll({path});
            for (const auto &change : edits) {
                if (change.first >= picks.size()) {
                    throw rfx::Error("the track is not in the file");
                }
                picks[change.first].edit = change.second;
            }
            rfx::PackOptions options;
            options.order = rfx::OrderMode::Given;
            rfx::assembleArchives({path}, picks, aside, options, std::nullopt);
        } catch (const std::exception &e) {
            *error = QString::fromUtf8(e.what());
        }
        *done = true;
    });
    {
        QProgressDialog busy(tr("Writing the file again..."), QString(), 0, 0, this);
        busy.setWindowTitle(tr("Edit"));
        busy.setWindowModality(Qt::WindowModal);
        busy.setCancelButton(nullptr);
        busy.setMinimumDuration(0);
        while (!*done) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(20);
        }
        worker.join();
    }
    QString failure = *error;
    if (failure.isEmpty() && QFile::exists(path) && !QFile::remove(path)) {
        failure = tr("%1 cannot be replaced: is it open in another program?").arg(QDir::toNativeSeparators(path));
    }
    if (failure.isEmpty() && !QFile::rename(aside, path)) {
        failure = tr("%1 cannot be put in place of the file").arg(QDir::toNativeSeparators(aside));
    }
    if (!failure.isEmpty()) {
        QFile::remove(aside);
    }
    // (the files are opened again, as they were, with the track that was selected)
    QString reopen;
    if (!m_session->open(sources, &reopen)) {
        say(failure.isEmpty() ? reopen : failure + QLatin1Char('\n') + reopen);
        return false;
    }
    m_session->setSelected(select);
    if (!failure.isEmpty()) {
        say(failure);
        return false;
    }
    statusBar()->showMessage(tr("%1 written again with what was changed").arg(info.fileName()), 8000);
    return true;
}

bool MainWindow::confirmCurves()
{
    const int unsaved = m_session->unsavedCurves();
    if (unsaved == 0) {
        return true;
    }
    QMessageBox box(QMessageBox::Question, tr("TBC curve"),
                    tr("The TBC curve of %n track(s) is worked out and is not kept in its .rfx file: it is lost when the file is closed. Keep it in the file first?", nullptr, unsaved),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Save);
    box.button(QMessageBox::Save)->setText(tr("Save"));
    box.button(QMessageBox::Discard)->setText(tr("Don't save"));
    switch (box.exec()) {
    case QMessageBox::Save: {
        QString error;
        if (m_session->saveSpeedCurves(&error) < 0) {
            QMessageBox::warning(this, tr("Cannot save the TBC curve"), error);
            return false;
        }
        return true;
    }
    case QMessageBox::Discard:
        return true;
    default:
        return false;
    }
}

void MainWindow::fixStandards()
{
    QList<StandardDoubt> list;
    for (const auto &doubt : m_session->takeStandardDoubts()) {
        if (!m_session->isOpen() || doubt.first >= m_session->trackCount() || !m_session->inArchive(doubt.first)) {
            continue;
        }
        const rfx::SourceMeta &sm = m_session->track(doubt.first);
        list.push_back(StandardDoubt{QString::fromStdString(sm.name), doubt.first, QString::fromStdString(sm.standard), doubt.second});
    }
    if (list.isEmpty()) {
        return;
    }
    StandardDialog dialog(list, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    std::map<QString, std::map<size_t, rfx::TrackEdit>> byFile;
    for (const auto &change : dialog.accepted()) {
        rfx::TrackEdit e;
        e.info.standard = change.second.toStdString();
        byFile[m_session->sourcePath(change.first)][m_session->localTrack(change.first)] = e;
    }
    // The curves that were just worked out are kept in their files first: writing a file again would lose them.
    QString error;
    m_session->saveSpeedCurves(&error);
    const int selected = m_session->selected();
    for (const auto &file : byFile) {
        if (!rewriteFile(file.first, file.second, selected)) {
            break;
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (confirmCurves()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::closeFile()
{
    if (confirmCurves()) {
        m_session->close();
    }
}

void MainWindow::updateVerify()
{
    bool archive = false;
    for (int k = 0; k < m_session->trackCount() && !archive; ++k) {
        archive = m_session->inArchive(k);
    }
    m_verifyAll->setEnabled(archive);
    m_verifyAction->setEnabled(archive);
    m_verifyTrack->setEnabled(m_session->isOpen() && m_session->selected() < m_session->trackCount() && m_session->inArchive(m_session->selected()));
    // (any track has lines to locate, a capture on its own too; not when it has its curve, or is being worked on)
    const int track = m_session->selected();
    m_editTrack->setEnabled(m_session->isOpen() && m_session->selected() < m_session->trackCount() && m_session->inArchive(m_session->selected()));
    m_computeTrack->setEnabled(m_session->isOpen() && track < m_session->trackCount() && !m_session->curveReady(track) && m_session->rfProgressOf(track) == -1);
}

bool MainWindow::verifyTracks(std::optional<int> track, bool quiet)
{
    if (!m_session->isOpen()) {
        return false;
    }
    // What is checked: the files that are archives, each whole, or the track alone.
    struct Part
    {
        QString path;
        std::optional<size_t> only;
    };
    std::vector<Part> parts;
    for (int k = 0; k < m_session->trackCount(); ++k) {
        if ((track && k != *track) || !m_session->inArchive(k)) {
            continue;
        }
        const QString path = m_session->sourcePath(k);
        if (track) {
            parts.push_back({path, m_session->localTrack(k)});
        } else if (std::none_of(parts.begin(), parts.end(), [&](const Part &p) { return p.path == path; })) {
            parts.push_back({path, std::nullopt});
        }
    }
    if (parts.empty()) {
        if (!quiet) {
            QMessageBox::information(this, tr("Check"), tr("There is nothing to check: a capture on its own keeps no checksum. Open an .rfx file."));
        }
        return false;
    }
    // The work is done on a thread of its own, with its own reading of the files; the window stays alive, and can stop it.
    struct Outcome
    {
        QString path;
        std::vector<rfx::TrackCheck> checks;
    };
    auto outcomes = std::make_shared<std::vector<Outcome>>();
    auto failure = std::make_shared<QString>();
    auto progress = std::make_shared<std::atomic<double>>(0.0);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([parts, outcomes, failure, progress, stop, done] {
        try {
            for (size_t i = 0; i < parts.size(); ++i) {
                Outcome o;
                o.path = parts[i].path;
                o.checks = rfx::verifyArchive(
                    parts[i].path, parts[i].only,
                    [&](double f) { *progress = (static_cast<double>(i) + f) / static_cast<double>(parts.size()); }, stop.get());
                outcomes->push_back(std::move(o));
            }
        } catch (const std::exception &e) {
            *failure = QString::fromUtf8(e.what());
        }
        *done = true;
    });
    QProgressDialog dialog(tr("Decoding and comparing with the checksums…"), tr("Stop"), 0, 1000, this);
    dialog.setWindowTitle(track ? tr("Checking track %1").arg(*track) : tr("Checking the files"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(0);
    dialog.setValue(0);
    while (!*done) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        dialog.setValue(static_cast<int>(*progress * 1000.0));
        if (dialog.wasCanceled()) {
            *stop = true;
        }
        QThread::msleep(20);
    }
    worker.join();
    dialog.reset();
    if (!failure->isEmpty()) {
        if (!quiet) {
            QMessageBox::critical(this, tr("Check"), *failure);
        }
        return false;
    }
    // What was found: the tracks that are not as packed, said by their name, and how many are.
    QStringList damaged;
    int intact = 0;
    int unchecked = 0;
    for (const Outcome &o : *outcomes) {
        for (const rfx::TrackCheck &c : o.checks) {
            if (!c.ok) {
                damaged << tr("%1 (track %2 of %3): %4").arg(QString::fromStdString(c.name)).arg(c.track).arg(QFileInfo(o.path).fileName(), QString::fromStdString(c.detail));
            } else if (c.checked) {
                ++intact;
            } else {
                ++unchecked;
            }
        }
    }
    const bool stopped = stop->load();
    QString text;
    if (!damaged.isEmpty()) {
        text = tr("Damaged: %n track(s) are not as they were packed:", nullptr, damaged.size()) + QLatin1Char('\n') + damaged.join(QLatin1Char('\n'));
    } else if (stopped) {
        text = tr("Stopped: the tracks checked before are intact (%n).", nullptr, intact);
    } else {
        text = tr("All intact: %n track(s) match the SHA-256 their file keeps of the samples.", nullptr, intact);
        if (unchecked > 0) {
            text += QLatin1Char('\n') + tr("%n track(s) have no checksum in their file, and nothing is said of them.", nullptr, unchecked);
        }
    }
    statusBar()->showMessage(text.section(QLatin1Char('\n'), 0, 0), 10000);
    if (!quiet) {
        if (damaged.isEmpty()) {
            QMessageBox::information(this, tr("Check"), text);
        } else {
            QMessageBox::warning(this, tr("Check"), text);
        }
    }
    return damaged.isEmpty() && !stopped && intact > 0;
}

void MainWindow::saveSpeed()
{
    if (!m_session->isOpen() || !m_session->canSaveSpeed()) {
        return;
    }
    int ready = 0;
    for (int k = 0; k < m_session->trackCount(); ++k) {
        ready += m_session->inArchive(k) && m_session->curveReady(k) ? 1 : 0;
    }
    if (ready == 0) {
        QMessageBox::information(this, tr("Save TBC curve"), tr("No TBC curve is worked out yet. Use Compute TBC curve (or tick From RF, or Align capture) for the tracks wanted, wait for it, then save."));
        return;
    }
    if (QMessageBox::question(this, tr("Save TBC curve"), tr("Add the TBC curve of %n track(s) to the .rfx file? It is written at the end of the file, nothing else in it changes.", nullptr, ready)) != QMessageBox::Yes) {
        return;
    }
    QString error;
    const int kept = m_session->saveSpeedCurves(&error);
    if (kept < 0) {
        QMessageBox::warning(this, tr("Cannot save the TBC curve"), error);
    } else {
        statusBar()->showMessage(tr("TBC curve kept for %n track(s).", nullptr, kept), 6000);
    }
}

void MainWindow::exportYaml()
{
    if (!m_session->isOpen()) {
        return;
    }
    // The metadata of the archive the selected track is in.
    const int k = m_session->selected();
    if (!m_session->inArchive(k)) {
        QMessageBox::information(this, tr("Cannot export"), tr("The selected track is a capture on its own: it has no metadata to export. Select a track of an .rfx file."));
        return;
    }
    const QString archive = m_session->sourcePath(k);
    const QString suggested = QFileInfo(archive).completeBaseName() + QStringLiteral(".yaml");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export the metadata"), suggested, tr("YAML (*.yaml)"));
    if (path.isEmpty()) {
        return;
    }
    try {
        const std::string text = rfx::archiveYaml(archive);
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size())) {
            throw rfx::Error("cannot write " + path.toStdString());
        }
    } catch (const std::exception &e) {
        QMessageBox::warning(this, tr("Cannot export"), QString::fromUtf8(e.what()));
    }
}

void MainWindow::updateRecent()
{
    m_recentMenu->clear();
    const QStringList recent = QSettings().value(QStringLiteral("recent")).toStringList();
    for (const QString &path : recent) {
        m_recentMenu->addAction(path, this, [this, path] { openFile(path); });
    }
    m_recentMenu->setEnabled(!recent.isEmpty());
}

// The files of a drop that the program can open: others (a .yaml, a .json, a folder) are left out.
static QStringList droppedRfFiles(const QMimeData *mime, int *ignored = nullptr)
{
    QStringList paths;
    int others = 0;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile() && isRfFile(url.toLocalFile())) {
            paths << url.toLocalFile();
        } else if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir() && !rfFilesInFolder(url.toLocalFile()).isEmpty()) {
            // A folder brings the RF files that are in it.
            paths << rfFilesInFolder(url.toLocalFile());
        } else {
            ++others;
        }
    }
    if (ignored) {
        *ignored = others;
    }
    return paths;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // Nothing to open in it: the drop is refused (the cursor says so).
    if (event->mimeData()->hasUrls() && !droppedRfFiles(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    dropFiles(event->mimeData());
}

// What a drop brings: the files that can be opened are added to the ones that are open.
void MainWindow::dropFiles(const QMimeData *mime)
{
    int ignored = 0;
    const QStringList paths = droppedRfFiles(mime, &ignored);
    if (paths.isEmpty()) {
        return;
    }
    if (ignored > 0) {
        statusBar()->showMessage(tr("%n dropped file(s) ignored: not an RF capture or an .rfx archive", nullptr, ignored), 6000);
    }
    openOrAdd(paths);
}

void MainWindow::setView(const QString &name)
{
    if (name == QLatin1String("individual") || name == QLatin1String("lanes")) {
        m_allLanes = name == QLatin1String("lanes");
        m_modeGroup->button(m_allLanes ? 1 : 0)->setChecked(true);
        m_tabs->setCurrentIndex(kTabIndividual);
        buildLanes();
    } else {
        m_tabs->setCurrentIndex(kTabCombined);
    }
}

bool MainWindow::saveScreenshot(const QString &path)
{
    show();
    QElapsedTimer timer;
    timer.start();
    // The worker is asked again by every change; wait until it has caught up.
    while ((!m_session->upToDate() || m_session->rfSpeedBusy() || timer.elapsed() < 400) && timer.elapsed() < 900000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    return grab().save(path);
}
