/******************************************************************************
 * mainwindow.h
 * rf-analyse - the window: the tracks, the metadata of the selected one, and the
 * views of the RF signal
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "edit.h"
#include "session.h"

#include <QMainWindow>

#include <functional>

#include <memory>
#include <vector>

class AppearanceMenus;
class DeviationBlock;
class MetadataPanel;
class QAbstractButton;
class QActionGroup;
class QButtonGroup;
class QCheckBox;
class QFrame;
class QHBoxLayout;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QMimeData;
class QMenu;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTabBar;
class QPushButton;
class QToolButton;
class QTemporaryDir;
class QVBoxLayout;
namespace rfx {
struct CaptureSpec;
}
class SpectrumView;
class StatsPanel;
class TrackList;
class WaveformView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // What File > Open and a drop do when files are already open: ask (the default), add or replace.
    enum class OpenMode { Ask, Add, Replace };
    void setOpenMode(OpenMode mode) { m_openMode = mode; }
    MainWindow();
    ~MainWindow() override;

    // Open `path`; false (after telling the user why) when it cannot be.
    bool openFile(const QString &path);
    // Several files together: their tracks come one after the other. The captures among
    // them are asked for once, in a table.
    bool openFiles(const QStringList &paths);
    // Files whose format, rate and bits are known; a compressed capture is decoded where it is looked at.
    // With `append`, the sources are added to the ones that are open.
    bool openSources(std::vector<Session::Source> sources, bool append = false);
    // What a drop brings: the files that can be opened are added to the ones that are open.
    void dropFiles(const QMimeData *mime);
    // The RF files of a folder, opened (or added, or replacing: asked when files are open).
    bool openFolder(const QString &folder);
    // Ask for more files and add them to the ones that are open.
    void addFiles();
    QString lastFolder() const;
    void rememberFolder(const QString &folder);
    void addPaths(const QStringList &paths);
    void openOrAdd(const QStringList &paths);
    bool sourcesFor(const QStringList &paths, std::vector<Session::Source> &sources);
    QStringList notOpenYet(const QStringList &paths);
    // One capture whose format and rate are known.
    bool openCapture(const QString &path, const rfx::CaptureSpec &spec);

    // "combined", "individual" (the selected track alone) or "lanes".
    void setView(const QString &name);
    // Start with the speed measured from the RF (the "From RF" box of the speed deviation).
    void setRfSpeed(bool on) { m_session->setRfSpeed(on); }
    // The track selected (0 to count - 1).
    void selectTrack(int k) { m_session->setSelected(k); }
    // The speed from the RF for every track at once.
    void setAlignAll() {
        for (int k = 0; k < m_session->trackCount(); ++k) {
            m_session->setAligned(k, true);
        }
    }
    // Check the tracks (the one given, or all of them) of the .rfx files that are open against the checksums they keep, decoding them. Says what it
    // found, unless `quiet`; true when all that was checked is as packed (and something was).
    bool verifyTracks(std::optional<int> track, bool quiet = false);
    // Write what `edit` changes of track `k` in its .rfx file: the file is written again aside (the samples are copied as they are, nothing is coded
    // again) and takes its place, and the files are opened again as they were. False, with the reason said unless `quiet`, when it could not be.
    bool rewriteTrack(int k, const rfx::TrackEdit &edit, bool quiet = false);
    void setRfSpeedAll() {
        for (int k = 0; k < m_session->trackCount(); ++k) {
            m_session->setRfSpeed(k, true);
        }
    }
    // Wait for the views to be up to date and write what the window shows to a
    // PNG file; false if it could not be written.
    bool saveScreenshot(const QString &path);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildMenus();
    QWidget *buildCombined();
    void buildLanes();
    QCheckBox *alignBox(QWidget *parent);
    void chooseFile();
    void chooseFolder();
    void exportYaml();
    void saveSpeed();
    void updateVerify();
    QAction *m_verifyAction = nullptr;
    QPushButton *m_verifyAll = nullptr;
    QPushButton *m_verifyTrack = nullptr;
    QPushButton *m_computeTrack = nullptr;
    QPushButton *m_editTrack = nullptr;
    void closeFile();
    // The name, the note and what is said of the disc and the signal, for the selected track: asked in a window, then written to its .rfx file.
    void editTrack();
    // What is worked out and not in its file is lost when the file is closed: asks whether to keep it. False when the person cancels.
    bool confirmCurves();
    // `follow`: the window with it (by the choice of the user, not when a file is opened or the compact mode comes).
    void showMetadata(bool shown, bool follow = false);
    void updateRecent();
    void rememberRecent(const QStringList &paths);
    void updateNavigation();
    void updateStatus();
    void updateTitle();
    void updateViews();
    void setAmplitude(double gain);
    void showGain(double gain);
    void showFftGain(double gain);
    void setNormaliseFft(bool on);
    QDoubleSpinBox *m_fftGain = nullptr;
    QWidget *m_fftBar = nullptr;
    QScrollArea *m_combinedScroll = nullptr;
    void setPanelVisible(int panel, bool on, bool keep);
    void setCompact(bool on);
    QAction *m_panelActions[4] = {};
    QAction *m_compactAction = nullptr;
    bool m_compact = false;
    bool m_compactApplied = false;
    bool m_beforeCompact[4] = {true, true, true, true};
    bool m_listBeforeCompact = true;
    bool m_infoBeforeCompact = true;
    QHBoxLayout *m_laneFftSlot = nullptr;
    void placeFftBar();
    void placeZoomBarOnly();
    QAction *m_normaliseAction = nullptr;

    Session *m_session;
    QStackedWidget *m_pages = nullptr;
    TrackList *m_tracks = nullptr;
    QWidget *m_panel = nullptr;
    MetadataPanel *m_metadata = nullptr;
    QLabel *m_panelTrack = nullptr;
    QTabBar *m_tabs = nullptr;
    QWidget *m_modeBox = nullptr;
    QButtonGroup *m_modeGroup = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QWidget *m_zoomBar = nullptr;
    QFrame *m_pane = nullptr;
    QHBoxLayout *m_waveHeadLayout = nullptr;
    QHBoxLayout *m_laneZoomSlot = nullptr;
    void placeZoomBar();
    void applyPanels();
    void resizeBy(int dy);
    void panelReducing(int panel, bool reduced, int bodyHeight);
    int panelExtent(int panel) const;
    int m_shownHeight[4] = {200, 200, 80, 140};
    bool m_followSize = true;
    bool m_normaliseFft = true;
    enum Panel { kPanelWave, kPanelSpectrum, kPanelDeviation, kPanelStats, kPanelCount };
    bool m_showPanel[kPanelCount] = {true, true, true, true};
    QWidget *m_panelHead[kPanelCount] = {};
    QWidget *m_panelBody[kPanelCount] = {};
    QWidget *m_panelSeparator[kPanelCount] = {};
    bool m_waveReduced = false;
    bool m_spectrumReduced = false;
    QHBoxLayout *m_spectrumHeadLayout = nullptr;
    QDoubleSpinBox *m_amplitude = nullptr;
    QStackedWidget *m_views = nullptr;

    // The combined view.
    WaveformView *m_wave = nullptr;
    SpectrumView *m_spectrum = nullptr;
    QLabel *m_spectrumWhere = nullptr;
    QComboBox *m_fft = nullptr;
    QCheckBox *m_bands = nullptr;
    DeviationBlock *m_deviation = nullptr;
    StatsPanel *m_stats = nullptr;

    // The individual view.
    QScrollArea *m_laneScroll = nullptr;
    QWidget *m_laneHost = nullptr;
    QVBoxLayout *m_laneLayout = nullptr;
    bool m_allLanes = false;
    // Which tracks the lanes were made for.
    QString m_laneSignature;

    // The navigation bar.
    QSlider *m_slider = nullptr;
    QLabel *m_position = nullptr;

    QLabel *m_statusTracks = nullptr;
    QLabel *m_statusDuration = nullptr;
    QLabel *m_statusSpeed = nullptr;
    QLabel *m_statusFile = nullptr;
    QMenu *m_recentMenu = nullptr;
    AppearanceMenus *m_appearance = nullptr;
    QAction *m_saveSpeedAction = nullptr;
    QCheckBox *m_alignBox = nullptr;
    QCheckBox *m_stackBox = nullptr;
    QCheckBox *m_residueBox = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_closeAction = nullptr;
    QAction *m_metadataAction = nullptr;
    bool m_panelShown = true;
    bool m_updating = false;
    // The raw copy of a FLAC capture that is open, removed when it is closed.
    std::vector<Session::Source> m_openSources;
    OpenMode m_openMode = OpenMode::Ask;
    QWidget *m_listColumn = nullptr;
    QHBoxLayout *m_tracksHeadLayout = nullptr;
    QHBoxLayout *m_panelHeadLayout = nullptr;
    QHBoxLayout *m_tabLayout = nullptr;
    void placeListToggle();
    QToolButton *m_listToggle = nullptr;
    QPushButton *m_infoButton = nullptr;
    void setListShown(bool shown);
    void resizeWidthBy(int dx);
    void resizeWithColumn(QWidget *column, int *remembered, bool show, const std::function<void(bool)> &change);
    int m_listWidth = 0;
    int m_panelWidth = 0;
};
