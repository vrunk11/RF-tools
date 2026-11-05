/******************************************************************************
 * session.h
 * rf-analyse - the file being looked at: its tracks, the window and the cursor,
 * and the spectra and statistics worked out for them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "analysis.h"
#include "timebase.h"
#include "trackreader.h"

#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// What the worker made for one state of the session.
struct AnalysisResult
{
    quint64 generation = 0;
    qint64 windowStart = 0;
    qint64 windowLength = 0;
    // For every track (empty for the ones not shown): the samples of the window in
    // the units of the converter with the most bits.
    std::vector<std::vector<float>> wave;
    // The spectrum and the statistics of the stretch the cursor or the zone stands
    // for.
    std::vector<rfx::Spectrum> spectra;
    std::vector<rfx::SampleStats> stats;
    // For every track: whether the stretch the spectrum and the statistics are made over
    // lies past the end of the track (or too little of it is inside), so that there is
    // nothing to say.
    std::vector<bool> pastEnd;
    qint64 analysedCenter = 0;
    qint64 analysedLength = 0;
    bool range = false;
    QString error;
};

// Where the analysis is made: the bar, or the zone that was dragged.
struct Selection
{
    qint64 center = 0;
    qint64 length = 0;
    bool range = false;
    qint64 first = 0;
    qint64 last = 0;
};

class Session : public QObject
{
    Q_OBJECT

public:
    // The window is never shorter than this, nor longer than this.
    static constexpr qint64 kShortestWindow = 256;
    static constexpr qint64 kLongestWindow = qint64(1) << 22;
    static constexpr qint64 kDefaultWindow = 16384;
    // The longest stretch a spectrum is made from.
    static constexpr qint64 kLongestAnalysis = qint64(1) << 22;

    explicit Session(QObject *parent = nullptr);
    ~Session() override;

    // One file to open: an .rfx archive, or a raw capture whose format and rate were
    // given. `shownAs` is the path to show when the file is a copy made to be read
    // (a decoded FLAC).
    struct Source
    {
        QString path;
        bool capture = false;
        rfx::CaptureSpec spec;
        QString shownAs;
    };

    // Open one file or several: their tracks come one after the other, in the order
    // given. False, with the reason, when one of them cannot be read; nothing is
    // opened then.
    bool open(const std::vector<Source> &sources, QString *error);
    bool open(const QString &path, QString *error);
    bool openCapture(const QString &path, const rfx::CaptureSpec &spec, QString *error, const QString &shownAs = QString());
    void close();
    bool isOpen() const { return !m_sources.empty(); }

    // The files, as shown, and the bytes they take.
    int sourceCount() const { return static_cast<int>(m_sources.size()); }
    QStringList paths() const;
    QString path() const { return m_sources.empty() ? QString() : m_sources.front().shownAs; }
    uint64_t fileSize() const;
    bool hasArchive() const;

    // The tracks.
    int trackCount() const;
    const rfx::SourceMeta &track(int k) const;
    QColor color(int k) const;
    bool visible(int k) const;
    void setVisible(int k, bool on);
    int selected() const { return m_selected; }
    void setSelected(int k);
    // Where a track comes from: from an archive (which may predict it from others) or
    // from a capture on its own; the path of that file, as opened; the number of its
    // first track among all; the bytes it takes (the whole file for a capture).
    bool inArchive(int k) const;
    QString sourcePath(int k) const;
    int firstTrackOfSource(int k) const;
    uint64_t storedBytes(int k) const;
    // The first reference of a track of an archive (among all the tracks), or -1 for
    // one that has none; the lag table against it, or null.
    int reference(int k) const;
    const rfx::LagTable *lagTable(int k) const;
    // A track of an archive that has no reference: the one the others of its file are
    // compared with, at the speed it was captured.
    bool isReference(int k) const;
    // The speed of a track of an archive against its reference at a sample, in parts
    // per million (see speedDeviationPpm): 0 for the reference itself, positive faster,
    // negative slower; the window it is measured over.
    double speedAt(int k, qint64 sample) const;
    qint64 speedWindow(int k) const;
    // The speed from the RF itself (off to begin with): the lines of every track are located from its RF (the pass 0 of ld-decode) and
    // the speed is the line spacing against the nominal one, in ppm, for every track whether it has a stored curve or not (the
    // reference and a capture on its own too). It is worked out in the background, a track after the other.
    // The speed measured from the RF, asked for track by track (the selected one for the forms without a track).
    bool rfSpeed(int k) const;
    void setRfSpeed(int k, bool on);
    // The waveform of a track with its time base corrected by that curve (it asks for the curve when there is none).
    bool aligned(int k) const;
    void setAligned(int k, bool on);
    // The curve is worked out (whether or not the graph of the speed shows it).
    bool curveReady(int k) const;
    // Keep the curves that are worked out in the .rfx files they are of (File > Save speed curve): the number of tracks kept; the files
    // that cannot take them are left alone. canSaveSpeed: there is such a file.
    bool canSaveSpeed() const;
    int saveSpeedCurves(QString *error);
    bool rfSpeed() const { return rfSpeed(m_selected); }
    void setRfSpeed(bool on) { setRfSpeed(m_selected, on); }
    // Whether some track's speed is still being worked out.
    bool rfSpeedBusy() const { return m_rfBusy.load() > 0; }
    // The tracks being worked on at the same time, and how far they are on average, in percent.
    int rfWorkingTracks() const;
    int rfMeanPercent() const;
    // Where the speed of track k stands for the list of tracks: -1 nothing is being done for it, -2 it waits for its turn, else the percent done.
    int rfProgressOf(int k) const;
    // Whether there is a speed to draw for the track (from the RF, once it is made; else against its reference).
    bool hasSpeed(int k) const;
    // Whether the speed of the track is 0 because it is the reference (not so when it is made from the RF).
    bool speedIsZero(int k) const { return !rfSpeed(k) && isReference(k); }
    // Bits the track carries, and the most of any track (what the waveform is in).
    int bits(int k) const;
    int displayBits() const { return m_displayBits; }

    // The capture.
    qint64 totalSamples() const { return m_total; }
    double rateHz() const { return m_rateHz; }
    bool rateKnown() const { return m_rateKnown; }
    double bandHighHz() const;
    double seconds(qint64 samples) const { return static_cast<double>(samples) / m_rateHz; }

    // The window the waveform shows.
    qint64 windowStart() const { return m_windowStart; }
    qint64 windowLength() const { return m_windowLength; }
    void setWindowStart(qint64 start);
    void scrollWindow(qint64 delta);
    // Zoom around `anchor` (a sample), keeping it where it is on the screen if it
    // is on it.
    void zoomBy(double factor, qint64 anchor);
    void resetZoom();
    // Move the window by `delta` and the bar with it.
    void pageBy(qint64 delta);
    void goToStart();
    // The window at the end of the longest track, with the bar at its last samples.
    void goToEnd();

    // The bar and the zone.
    Selection selection() const;
    void setCursor(qint64 sample, bool follow = false);
    void setRange(qint64 a, qint64 b);
    int fftSize() const { return m_fftSize; }
    void setFftSize(int size);

    const AnalysisResult &result() const { return m_result; }
    // A compressed capture is decoded where it is looked at, which takes a moment: so that something is shown while it
    // does, a rough spectrum of the whole capture is made in the background (a few places, a short stretch at each, from a
    // small number of reads). The one of the track `k` nearest to the bar, or null when there is none (yet, or for a track
    // that is read quickly).
    std::shared_ptr<const rfx::Spectrum> coarseSpectrum(int k) const;
    // Whether the rough one is to be shown now: the result for the state now is late.
    bool coarseWanted() const;
    // Whether the result is the one for the state now (nothing is being worked on).
    bool upToDate() const { return m_result.generation == m_generation.load(); }

signals:
    void opened();
    void closed();
    // Anything the views draw from changed (the window, the bar, which tracks).
    void changed();
    // The worker has a new result.
    void resultReady();
    // The result for the state now is late (the views may show the rough spectrum meanwhile).
    void staleResult();
    // The rough spectra have a new one.
    void overviewReady();
    // The speed from the RF was switched, or one more track of it is made.
    void rfSpeedChanged();
    void rfSpeedProgress();

private:
    void refresh();
    void clampWindow();
    void startOverview();
    void startRfSpeed(int only);
    std::shared_ptr<const std::vector<std::pair<double, double>>> curveOf(int k) const;
    double rfSpeedAt(int k, qint64 sample) const;

    struct Loaded
    {
        std::unique_ptr<rfx::TrackReader> reader;
        QString shownAs;
        int first = 0;
    };
    struct TrackRef
    {
        int source = 0;
        size_t local = 0;
    };
    std::vector<Loaded> m_sources;
    std::vector<TrackRef> m_tracks;
    std::vector<bool> m_visible;
    int m_selected = 0;
    int m_displayBits = 10;
    qint64 m_total = 0;
    double m_rateHz = 75e6;
    bool m_rateKnown = false;
    qint64 m_windowStart = 0;
    qint64 m_windowLength = kDefaultWindow;
    qint64 m_cursor = 0;
    qint64 m_rangeEnd = -1;
    int m_fftSize = 4096;
    AnalysisResult m_result;
    std::atomic<quint64> m_generation{0};
    // The generation of the result that was last taken in.
    std::atomic<quint64> m_applied{0};
    QThreadPool m_pool;
    // The rough spectra of the tracks, made by a thread of their own; `m_openId` changes when the files change, which
    // ends what that was doing.
    struct Overview
    {
        std::vector<qint64> centers;
        std::vector<std::shared_ptr<const rfx::Spectrum>> spectra;
    };
    QThreadPool m_overviewPool;
    // The speed from the RF: per track, the speed in ppm at the centre of each field (sample, ppm), or null while it is not made.
    using SpeedCurve = std::vector<std::pair<double, double>>;
    std::vector<bool> m_rfOn;
    std::vector<bool> m_aligned;
    bool m_alignPlaced = false;
    void placeAligned();
    QThreadPool m_speedPool;
    std::vector<std::shared_ptr<const SpeedCurve>> m_rfCurves;
    // What pass 0 found in each track (the fields), from which the curve above and the time base are made.
    struct RfData
    {
        std::vector<rfx::Field> fields;
        bool pal = false;
        double rate = 0.0;
    };
    std::vector<std::shared_ptr<const RfData>> m_rfData;
    std::vector<std::shared_ptr<const TimeBase>> m_timebases;
    std::shared_ptr<const TimeBase> timebaseOf(int k);
    std::vector<bool> m_rfStarted;
    std::atomic<int> m_rfBusy{0};
    // Per track, the percent done while it is being worked on (-1 otherwise).
    std::unique_ptr<std::atomic<int>[]> m_rfPct;
    size_t m_rfPctCount = 0;
    std::atomic<bool> m_speedStop{false};
    mutable std::mutex m_overviewMutex;
    std::vector<std::shared_ptr<const Overview>> m_overview;
    std::atomic<quint64> m_openId{0};
    QElapsedTimer m_waiting;
    QTimer m_staleTimer;
};
