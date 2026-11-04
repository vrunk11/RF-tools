/******************************************************************************
 * session.cpp
 * rf-analyse - the files being looked at
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "session.h"

#include "error.h"
#include "rfsync.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

namespace {

// One colour a track, in the order of the tracks (they are told apart, and keep
// their colour while others are hidden).
// The colours of ld-stacker-gui for its sources.
const char *const kTrackColors[] = {"#378ADD", "#1D9E75", "#D85A30", "#7F77DD", "#BA7517", "#D4537E", "#639922", "#E24B4A"};

constexpr double kDefaultRateHz = 75.0e6;
constexpr qint64 kMinimumRange = 64;
// How long the result for the state now may be late before the rough spectrum is shown.
constexpr int kCoarseAfterMs = 80;
// The rough spectrum: this many places over the capture, a stretch of this many samples at each.
constexpr int kOverviewPlaces = 128;
constexpr qint64 kOverviewStretch = 16384;
constexpr int kOverviewFft = 4096;

// Where the samples of a track are read.
struct TrackSource
{
    rfx::TrackReader *reader = nullptr;
    size_t local = 0;
};

// What the worker is asked for.
struct Request
{
    quint64 generation = 0;
    qint64 windowStart = 0;
    qint64 windowLength = 0;
    qint64 analysedCenter = 0;
    qint64 analysedLength = 0;
    bool range = false;
    int fftSize = 4096;
    int displayBits = 10;
    double rateHz = kDefaultRateHz;
    std::vector<bool> visible;
    std::vector<TrackSource> tracks;
};

AnalysisResult compute(const Request &q, const std::atomic<quint64> &current)
{
    AnalysisResult r;
    r.generation = q.generation;
    r.windowStart = q.windowStart;
    r.windowLength = q.windowLength;
    r.analysedCenter = q.analysedCenter;
    r.analysedLength = q.analysedLength;
    r.range = q.range;
    const size_t tracks = q.tracks.size();
    r.wave.resize(tracks);
    r.spectra.resize(tracks);
    r.stats.resize(tracks);
    r.pastEnd.assign(tracks, false);

    // What is made for one track: its samples over the window, and its spectrum and statistics over the stretch.
    const auto work = [&](size_t k) {
        std::vector<int32_t> raw;
        rfx::TrackReader &reader = *q.tracks[k].reader;
        const size_t local = q.tracks[k].local;
        const rfx::SourceMeta &sm = reader.meta().sources[local];
        const int ownBits = rfx::carriedBits(sm.format(), sm.shift);

        raw.assign(static_cast<size_t>(q.windowLength), 0);
        reader.read(local, q.windowStart, raw.size(), raw.data());
        r.wave[k].resize(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            r.wave[k][i] = static_cast<float>(rfx::toConverterUnits(sm.format(), raw[i], q.displayBits));
        }

        if (current.load() != q.generation) {
            return;
        }
        // Only what is in the track: a stretch that goes over its ends is cut, not
        // padded with zeros, which would be taken for signal. Too little left, and there
        // is nothing to say.
        const qint64 from = q.analysedCenter - q.analysedLength / 2;
        const qint64 lo = std::max<qint64>(from, 0);
        const qint64 hi = std::min<qint64>(from + q.analysedLength, static_cast<qint64>(sm.words));
        if (hi - lo < kMinimumRange) {
            r.pastEnd[k] = true;
            return;
        }
        raw.assign(static_cast<size_t>(hi - lo), 0);
        reader.read(local, lo, raw.size(), raw.data());
        std::vector<double> x(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            x[i] = rfx::toConverterUnits(sm.format(), raw[i], ownBits);
        }
        const double rate = sm.rateHz > 0 ? sm.rateHz : q.rateHz;
        r.spectra[k] = rfx::analyseSpectrum(x, static_cast<size_t>(q.fftSize), rate, std::ldexp(1.0, ownBits - 1));
        r.stats[k] = rfx::computeStats(x, r.spectra[k], rfx::defaultBandHighHz(rate));
    };

    // The tracks of compressed captures are decoded by ffmpeg, a process for each: those of several are asked for at the same
    // time (the others are read one after the other, as they were).
    bool compressed = false;
    for (size_t k = 0; k < tracks; ++k) {
        compressed = compressed || (q.visible[k] && q.tracks[k].reader->isCompressed());
    }
    std::atomic<size_t> next{0};
    std::mutex failureMutex;
    QString failure;
    const auto body = [&] {
        for (;;) {
            const size_t k = next.fetch_add(1);
            if (k >= tracks || current.load() != q.generation) {
                return;
            }
            if (!q.visible[k]) {
                continue;
            }
            try {
                work(k);
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lock(failureMutex);
                if (failure.isEmpty()) {
                    failure = QString::fromUtf8(e.what());
                }
            }
        }
    };
    const unsigned workers = compressed ? std::clamp(std::thread::hardware_concurrency(), 2U, 8U) : 1U;
    std::vector<std::thread> helpers;
    for (unsigned i = 1; i < std::min<size_t>(workers, tracks); ++i) {
        helpers.emplace_back(body);
    }
    body();
    for (std::thread &helper : helpers) {
        helper.join();
    }
    r.error = failure;
    return r;
}

} // namespace

Session::Session(QObject *parent) : QObject(parent)
{
    // One worker: a new state of the window supersedes the one being worked on.
    m_pool.setMaxThreadCount(1);
    m_overviewPool.setMaxThreadCount(1);
    // When the result is late, the views are told once (they may then show the rough spectrum).
    m_staleTimer.setSingleShot(true);
    connect(&m_staleTimer, &QTimer::timeout, this, [this] {
        if (!upToDate()) {
            emit staleResult();
        }
    });
}


Session::~Session()
{
    close();
}

bool Session::open(const QString &path, QString *error)
{
    Source s;
    s.path = path;
    return open(std::vector<Source>{s}, error);
}

bool Session::openCapture(const QString &path, const rfx::CaptureSpec &spec, QString *error, const QString &shownAs)
{
    Source s;
    s.path = path;
    s.capture = true;
    s.spec = spec;
    s.shownAs = shownAs;
    return open(std::vector<Source>{s}, error);
}

bool Session::open(const std::vector<Source> &sources, QString *error)
{
    if (sources.empty()) {
        if (error) {
            *error = tr("No file was given.");
        }
        return false;
    }
    // Everything is read before anything is closed: one that cannot be leaves what
    // is open as it is.
    std::vector<Loaded> loaded;
    for (const Source &s : sources) {
        try {
            Loaded l;
            l.reader = s.capture ? std::make_unique<rfx::TrackReader>(s.path, s.spec) : std::make_unique<rfx::TrackReader>(s.path);
            l.shownAs = s.shownAs.isEmpty() ? s.path : s.shownAs;
            loaded.push_back(std::move(l));
        } catch (const std::exception &e) {
            if (error) {
                *error = sources.size() > 1 ? tr("%1: %2").arg(QFileInfo(s.path).fileName(), QString::fromUtf8(e.what())) : QString::fromUtf8(e.what());
            }
            return false;
        }
    }
    close();
    m_sources = std::move(loaded);
    m_tracks.clear();
    m_total = 0;
    m_displayBits = 1;
    m_rateKnown = false;
    m_rateHz = kDefaultRateHz;
    for (size_t s = 0; s < m_sources.size(); ++s) {
        m_sources[s].first = static_cast<int>(m_tracks.size());
        const rfx::ArchiveMeta &meta = m_sources[s].reader->meta();
        for (size_t local = 0; local < meta.sources.size(); ++local) {
            const rfx::SourceMeta &sm = meta.sources[local];
            m_tracks.push_back({static_cast<int>(s), local});
            m_total = std::max<qint64>(m_total, static_cast<qint64>(sm.words));
            m_displayBits = std::max(m_displayBits, rfx::carriedBits(sm.format(), sm.shift));
            if (!m_rateKnown && sm.rateHz > 0) {
                m_rateHz = sm.rateHz;
                m_rateKnown = true;
            }
        }
    }
    m_visible.assign(m_tracks.size(), true);
    m_selected = 0;
    m_windowLength = std::min(kDefaultWindow, std::max<qint64>(m_total, 1));
    m_windowStart = 0;
    m_cursor = m_windowLength / 2;
    m_rangeEnd = -1;
    m_result = AnalysisResult();
    m_speedStop = false;
    emit opened();
    refresh();
    startOverview();
    if (m_rfSpeed) {
        startRfSpeed();
    }
    return true;
}

void Session::close()
{
    if (m_sources.empty()) {
        return;
    }
    ++m_generation;
    ++m_openId;
    m_pool.waitForDone();
    m_overviewPool.waitForDone();
    {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        m_overview.clear();
        m_rfCurves.clear();
        m_rfStarted.clear();
    }
    m_sources.clear();
    m_tracks.clear();
    m_result = AnalysisResult();
    m_rfBusy = 0;
    emit closed();
}

QStringList Session::paths() const
{
    QStringList out;
    for (const Loaded &l : m_sources) {
        out << l.shownAs;
    }
    return out;
}

uint64_t Session::fileSize() const
{
    uint64_t total = 0;
    for (const Loaded &l : m_sources) {
        total += l.reader->fileSize();
    }
    return total;
}

bool Session::hasArchive() const
{
    return std::any_of(m_sources.begin(), m_sources.end(), [](const Loaded &l) { return l.reader->isArchive(); });
}

int Session::trackCount() const
{
    return static_cast<int>(m_tracks.size());
}

const rfx::SourceMeta &Session::track(int k) const
{
    const TrackRef &t = m_tracks[static_cast<size_t>(k)];
    return m_sources[static_cast<size_t>(t.source)].reader->meta().sources[t.local];
}

QColor Session::color(int k) const
{
    return QColor(QLatin1String(kTrackColors[k % (sizeof(kTrackColors) / sizeof(kTrackColors[0]))]));
}

bool Session::visible(int k) const
{
    return k >= 0 && k < static_cast<int>(m_visible.size()) && m_visible[static_cast<size_t>(k)];
}

void Session::setVisible(int k, bool on)
{
    if (k < 0 || k >= static_cast<int>(m_visible.size()) || m_visible[static_cast<size_t>(k)] == on) {
        return;
    }
    m_visible[static_cast<size_t>(k)] = on;
    emit changed();
    refresh();
}

void Session::setSelected(int k)
{
    if (k < 0 || k >= trackCount() || k == m_selected) {
        return;
    }
    m_selected = k;
    emit changed();
}

bool Session::inArchive(int k) const
{
    return m_sources[static_cast<size_t>(m_tracks[static_cast<size_t>(k)].source)].reader->isArchive();
}

QString Session::sourcePath(int k) const
{
    return m_sources[static_cast<size_t>(m_tracks[static_cast<size_t>(k)].source)].shownAs;
}

int Session::firstTrackOfSource(int k) const
{
    return m_sources[static_cast<size_t>(m_tracks[static_cast<size_t>(k)].source)].first;
}

uint64_t Session::storedBytes(int k) const
{
    const TrackRef &t = m_tracks[static_cast<size_t>(k)];
    return m_sources[static_cast<size_t>(t.source)].reader->packedBytes(t.local);
}

int Session::reference(int k) const
{
    const rfx::SourceMeta &sm = track(k);
    return sm.refs.empty() ? -1 : firstTrackOfSource(k) + static_cast<int>(sm.refs.front());
}

const rfx::LagTable *Session::lagTable(int k) const
{
    const TrackRef &t = m_tracks[static_cast<size_t>(k)];
    const rfx::SourceMeta &sm = track(k);
    if (sm.refs.empty()) {
        return nullptr;
    }
    return m_sources[static_cast<size_t>(t.source)].reader->lagTable(t.local, sm.refs.front());
}

bool Session::isReference(int k) const
{
    return inArchive(k) && track(k).refs.empty();
}

int Session::bits(int k) const
{
    const rfx::SourceMeta &sm = track(k);
    return rfx::carriedBits(sm.format(), sm.shift);
}

double Session::bandHighHz() const
{
    return rfx::defaultBandHighHz(m_rateHz);
}

void Session::clampWindow()
{
    m_windowLength = std::clamp<qint64>(m_windowLength, kShortestWindow, std::max(kShortestWindow, std::min(kLongestWindow, m_total)));
    m_windowStart = std::clamp<qint64>(m_windowStart, 0, std::max<qint64>(0, m_total - m_windowLength));
}

void Session::setWindowStart(qint64 start)
{
    if (!isOpen()) {
        return;
    }
    m_windowStart = start;
    clampWindow();
    emit changed();
    refresh();
}

void Session::scrollWindow(qint64 delta)
{
    setWindowStart(m_windowStart + delta);
}

void Session::zoomBy(double factor, qint64 anchor)
{
    if (!isOpen()) {
        return;
    }
    const double rel = static_cast<double>(anchor - m_windowStart) / static_cast<double>(m_windowLength);
    const qint64 old = m_windowLength;
    m_windowLength = static_cast<qint64>(std::llround(static_cast<double>(m_windowLength) * factor));
    clampWindow();
    if (m_windowLength == old) {
        return;
    }
    m_windowStart = (rel >= 0.0 && rel <= 1.0) ? anchor - static_cast<qint64>(rel * static_cast<double>(m_windowLength)) : anchor - m_windowLength / 2;
    clampWindow();
    emit changed();
    refresh();
}

void Session::resetZoom()
{
    if (!isOpen()) {
        return;
    }
    const qint64 c = selection().center;
    m_windowLength = kDefaultWindow;
    clampWindow();
    m_windowStart = c - m_windowLength / 2;
    clampWindow();
    emit changed();
    refresh();
}

void Session::pageBy(qint64 delta)
{
    if (!isOpen()) {
        return;
    }
    const qint64 before = m_windowStart;
    m_windowStart += delta;
    clampWindow();
    const qint64 moved = m_windowStart - before;
    m_cursor = std::clamp<qint64>(m_cursor + moved, 0, m_total - 1);
    m_rangeEnd = -1;
    emit changed();
    refresh();
}

void Session::goToStart()
{
    if (!isOpen()) {
        return;
    }
    // The window at the start, and the bar where the stretch it stands for begins at the
    // first sample.
    m_windowStart = 0;
    m_cursor = std::min<qint64>(m_fftSize / 2, std::max<qint64>(0, m_total - 1));
    m_rangeEnd = -1;
    emit changed();
    refresh();
}

void Session::goToEnd()
{
    if (!isOpen()) {
        return;
    }
    // The window at the end, and the bar where the stretch it stands for ends at the last
    // sample.
    m_windowStart = m_total;
    clampWindow();
    m_cursor = std::max<qint64>(0, m_total - 1 - m_fftSize / 2);
    m_rangeEnd = -1;
    emit changed();
    refresh();
}

Selection Session::selection() const
{
    Selection s;
    if (m_rangeEnd >= 0) {
        s.range = true;
        s.first = std::min(m_cursor, m_rangeEnd);
        s.last = std::max(m_cursor, m_rangeEnd);
        s.center = (s.first + s.last) / 2;
        s.length = std::max<qint64>(kMinimumRange, s.last - s.first);
    } else {
        s.center = m_cursor;
        s.length = m_fftSize;
        s.first = m_cursor - m_fftSize / 2;
        s.last = m_cursor + m_fftSize / 2;
    }
    return s;
}

void Session::setCursor(qint64 sample, bool follow)
{
    if (!isOpen()) {
        return;
    }
    m_cursor = std::clamp<qint64>(sample, 0, std::max<qint64>(0, m_total - 1));
    m_rangeEnd = -1;
    if (follow && (m_cursor < m_windowStart || m_cursor > m_windowStart + m_windowLength)) {
        m_windowStart = m_cursor - m_windowLength / 2;
        clampWindow();
    }
    emit changed();
    refresh();
}

void Session::setRange(qint64 a, qint64 b)
{
    if (!isOpen()) {
        return;
    }
    a = std::clamp<qint64>(a, 0, m_total - 1);
    b = std::clamp<qint64>(b, 0, m_total - 1);
    m_cursor = a;
    m_rangeEnd = std::llabs(b - a) >= kMinimumRange ? b : -1;
    emit changed();
    refresh();
}

void Session::setFftSize(int size)
{
    if (size == m_fftSize) {
        return;
    }
    m_fftSize = size;
    emit changed();
    refresh();
}

void Session::refresh()
{
    if (!isOpen()) {
        return;
    }
    const quint64 generation = ++m_generation;
    m_waiting.start();
    m_staleTimer.start(kCoarseAfterMs);
    Request q;
    q.generation = generation;
    q.windowStart = m_windowStart;
    q.windowLength = m_windowLength;
    const Selection s = selection();
    q.analysedCenter = s.center;
    q.analysedLength = std::min(s.length, kLongestAnalysis);
    q.range = s.range;
    q.fftSize = m_fftSize;
    q.displayBits = m_displayBits;
    q.rateHz = m_rateHz;
    q.visible = m_visible;
    for (const TrackRef &t : m_tracks) {
        q.tracks.push_back({m_sources[static_cast<size_t>(t.source)].reader.get(), t.local});
    }
    m_pool.start(QRunnable::create([this, q]() {
        if (m_generation.load() != q.generation) {
            return;
        }
        auto result = std::make_shared<AnalysisResult>(compute(q, m_generation));
        QMetaObject::invokeMethod(
            this,
            [this, result]() {
                if (result->generation == m_generation.load()) {
                    m_applied = result->generation;
                    m_result = std::move(*result);
                    emit resultReady();
                }
            },
            Qt::QueuedConnection);
    }));
}

// The rough spectra of the compressed captures, in the background: for each, a short stretch at a number of places over it,
// read where it is (a jump and a short decoding for each). It waits while a result is being worked on, so as not to slow it.
void Session::startOverview()
{
    {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        m_overview.assign(m_tracks.size(), nullptr);
    }
    struct Job
    {
        rfx::TrackReader *reader;
        size_t local;
        int track;
    };
    std::vector<Job> jobs;
    for (size_t k = 0; k < m_tracks.size(); ++k) {
        rfx::TrackReader *reader = m_sources[static_cast<size_t>(m_tracks[k].source)].reader.get();
        if (reader->isCompressed()) {
            jobs.push_back({reader, m_tracks[k].local, static_cast<int>(k)});
        }
    }
    if (jobs.empty()) {
        return;
    }
    const quint64 id = m_openId.load();
    const double fallbackRate = m_rateHz;
    m_overviewPool.start(QRunnable::create([this, jobs, id, fallbackRate]() {
        const auto stale = [this, id] { return m_openId.load() != id; };
        for (const Job &job : jobs) {
            const rfx::SourceMeta &sm = job.reader->meta().sources[job.local];
            const int bits = rfx::carriedBits(sm.format(), sm.shift);
            const double rate = sm.rateHz > 0 ? sm.rateHz : fallbackRate;
            const qint64 words = static_cast<qint64>(sm.words);
            auto overview = std::make_shared<Overview>();
            std::vector<int32_t> raw(static_cast<size_t>(kOverviewStretch));
            std::vector<double> x(raw.size());
            try {
                for (int i = 0; i < kOverviewPlaces; ++i) {
                    // Not while the window is being worked out: what is looked at comes first.
                    while (!stale() && m_generation.load() != m_applied.load()) {
                        QThread::msleep(15);
                    }
                    if (stale()) {
                        return;
                    }
                    const qint64 center = (2 * static_cast<qint64>(i) + 1) * words / (2 * kOverviewPlaces);
                    const qint64 from = std::clamp<qint64>(center - kOverviewStretch / 2, 0, std::max<qint64>(0, words - kOverviewStretch));
                    const size_t n = static_cast<size_t>(std::min<qint64>(kOverviewStretch, words - from));
                    if (n < static_cast<size_t>(kOverviewFft)) {
                        continue;
                    }
                    job.reader->readOnce(job.local, from, n, raw.data());
                    x.resize(n);
                    for (size_t j = 0; j < n; ++j) {
                        x[j] = rfx::toConverterUnits(sm.format(), raw[j], bits);
                    }
                    overview->centers.push_back(from + static_cast<qint64>(n) / 2);
                    overview->spectra.push_back(std::make_shared<const rfx::Spectrum>(rfx::analyseSpectrum(x, static_cast<size_t>(kOverviewFft), rate, std::ldexp(1.0, bits - 1))));
                }
            } catch (const std::exception &) {
                continue;   // (this track has no rough spectrum)
            }
            if (stale()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_overviewMutex);
                if (static_cast<size_t>(job.track) < m_overview.size()) {
                    m_overview[static_cast<size_t>(job.track)] = overview;
                }
            }
            QMetaObject::invokeMethod(this, [this, id] {
                if (m_openId.load() == id) {
                    emit overviewReady();
                }
            }, Qt::QueuedConnection);
        }
    }));
}

std::shared_ptr<const rfx::Spectrum> Session::coarseSpectrum(int k) const
{
    std::shared_ptr<const Overview> overview;
    {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        if (k < 0 || static_cast<size_t>(k) >= m_overview.size()) {
            return nullptr;
        }
        overview = m_overview[static_cast<size_t>(k)];
    }
    if (!overview || overview->centers.empty()) {
        return nullptr;
    }
    const qint64 at = selection().center;
    size_t best = 0;
    for (size_t i = 1; i < overview->centers.size(); ++i) {
        if (std::llabs(overview->centers[i] - at) < std::llabs(overview->centers[best] - at)) {
            best = i;
        }
    }
    return overview->spectra[best];
}

bool Session::coarseWanted() const
{
    return !upToDate() && m_waiting.isValid() && m_waiting.elapsed() >= kCoarseAfterMs;
}

void Session::setRfSpeed(bool on)
{
    if (on == m_rfSpeed) {
        return;
    }
    m_rfSpeed = on;
    if (on) {
        startRfSpeed();
    }
    emit rfSpeedChanged();
}

bool Session::hasSpeed(int k) const
{
    if (m_rfSpeed) {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        return static_cast<size_t>(k) < m_rfCurves.size() && m_rfCurves[static_cast<size_t>(k)] != nullptr;
    }
    return lagTable(k) != nullptr;
}

// The lines of every track are located from its RF, one track after the other on a thread of its own (the filters use all the
// processors); what is made stays until the files change.
void Session::startRfSpeed()
{
    struct Job
    {
        rfx::TrackReader *reader;
        size_t local;
        int track;
        double rate;
    };
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        m_rfCurves.resize(m_tracks.size());
        m_rfStarted.resize(m_tracks.size(), false);
        for (size_t k = 0; k < m_tracks.size(); ++k) {
            if (m_rfStarted[k]) {
                continue;
            }
            m_rfStarted[k] = true;
            rfx::TrackReader *reader = m_sources[static_cast<size_t>(m_tracks[k].source)].reader.get();
            const double rate = reader->meta().sources[m_tracks[k].local].rateHz;
            jobs.push_back({reader, m_tracks[k].local, static_cast<int>(k), rate > 0 ? rate : m_rateHz});
        }
    }
    if (jobs.empty()) {
        return;
    }
    const quint64 id = m_openId.load();
    m_rfBusy += static_cast<int>(jobs.size());
    m_speedPool.setMaxThreadCount(1);
    m_speedPool.start(QRunnable::create([this, jobs, id]() {
        for (const Job &job : jobs) {
            const auto stale = [this, id] { return m_openId.load() != id || m_speedStop.load(); };
            std::shared_ptr<SpeedCurve> curve;
            if (!stale()) {
                try {
                    // NTSC first; a PAL capture gives no field as NTSC.
                    for (const rfx::VideoSystem system : {rfx::VideoSystem::Ntsc, rfx::VideoSystem::Pal}) {
                        const std::vector<rfx::Field> fields = rfx::locateFields(*job.reader, job.local, job.rate, system, 0, &m_speedStop);
                        const double nominal = rfx::nominalLineSamples(job.rate, system);
                        auto made = std::make_shared<SpeedCurve>();
                        for (const rfx::Field &f : fields) {
                            if (f.lines.size() < 20) {
                                continue;
                            }
                            // The line spacing over the field against the nominal one: shorter lines are a faster disc.
                            const double spacing = (f.lines.back() - f.lines.front()) / static_cast<double>(f.lines.size() - 1);
                            made->emplace_back((f.lines.front() + f.lines.back()) / 2.0, (nominal / spacing - 1.0) * 1e6);
                        }
                        if (!made->empty()) {
                            curve = made;
                            break;
                        }
                        if (stale()) {
                            break;
                        }
                    }
                } catch (const std::exception &) {
                    // (this track has no speed to show)
                }
            }
            if (!stale()) {
                if (!curve) {
                    curve = std::make_shared<SpeedCurve>();
                }
                std::lock_guard<std::mutex> lock(m_overviewMutex);
                if (static_cast<size_t>(job.track) < m_rfCurves.size()) {
                    m_rfCurves[static_cast<size_t>(job.track)] = curve;
                }
            }
            --m_rfBusy;
            QMetaObject::invokeMethod(this, [this, id] {
                if (m_openId.load() == id) {
                    emit rfSpeedChanged();
                }
            }, Qt::QueuedConnection);
        }
    }));
}

double Session::rfSpeedAt(int k, qint64 sample) const
{
    std::shared_ptr<const SpeedCurve> curve;
    {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        if (static_cast<size_t>(k) < m_rfCurves.size()) {
            curve = m_rfCurves[static_cast<size_t>(k)];
        }
    }
    if (!curve || curve->empty()) {
        return 0.0;
    }
    const double x = static_cast<double>(sample);
    const auto next = std::lower_bound(curve->begin(), curve->end(), x, [](const std::pair<double, double> &p, double v) { return p.first < v; });
    if (next == curve->begin()) {
        return next->second;
    }
    if (next == curve->end()) {
        return curve->back().second;
    }
    const auto before = next - 1;
    const double t = (x - before->first) / (next->first - before->first);
    return before->second + t * (next->second - before->second);
}

qint64 Session::speedWindow(int k) const

{
    // A few hundredths of the track, and never less than a few places of its table:
    // long enough for the lag to have moved by more than its rounding.
    const rfx::LagTable *t = lagTable(k);
    const qint64 words = static_cast<qint64>(track(k).words);
    const qint64 gap = t && !t->pos.empty() ? words / static_cast<qint64>(t->pos.size()) : 0;
    return std::max<qint64>(words / 200, 3 * gap);
}

double Session::speedAt(int k, qint64 sample) const
{
    if (m_rfSpeed) {
        return rfSpeedAt(k, sample);
    }
    const rfx::LagTable *t = lagTable(k);
    if (!t) {
        return 0.0;
    }
    const qint64 last = static_cast<qint64>(track(k).words) - 1;
    const qint64 half = speedWindow(k) / 2;
    const qint64 from = std::clamp<qint64>(sample - half, 0, last);
    const qint64 to = std::clamp<qint64>(sample + half, 0, last);
    return to > from ? static_cast<double>(t->lagAt(to) - t->lagAt(from)) / static_cast<double>(to - from) * 1e6 : 0.0;
}
