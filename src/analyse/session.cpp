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

#include "timebase.h"

#include "error.h"
#include "rfsync.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>

#include <algorithm>
#include <array>
#include <map>
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
    // When the track is shown with its time base corrected: where its lines are against the corrected time.
    std::shared_ptr<const TimeBase> base;
};
// The four points around a place, a cubic through them (Catmull-Rom): the sample between the second and the third at `t`.
float cubic(float a, float b, float c, float d, double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    return static_cast<float>(0.5 * ((2.0 * b) + (-a + c) * t + (2.0 * a - 5.0 * b + 4.0 * c - d) * t2 + (-a + 3.0 * b - 3.0 * c + d) * t3));
}

// The samples of a track over [start, start + count) of its corrected time, in the units of a converter of `bits` bits: the track is read
// where its corrected time is (a cubic through four samples), and is zero where it is not there.
std::vector<double> readCorrected(rfx::TrackReader &reader, size_t local, const rfx::SourceMeta &sm, const TimeBase &base, qint64 start, qint64 count, int bits, std::vector<char> *valid = nullptr)
{
    const double first = base.sourceAt(static_cast<double>(start));
    const double last = base.sourceAt(static_cast<double>(start + count - 1));
    const qint64 from = std::max<qint64>(static_cast<qint64>(std::floor(std::min(first, last))) - 2, 0);
    const qint64 to = std::min<qint64>(static_cast<qint64>(std::ceil(std::max(first, last))) + 3, static_cast<qint64>(sm.words));
    std::vector<float> source;
    if (to > from) {
        std::vector<int32_t> raw(static_cast<size_t>(to - from), 0);
        reader.read(local, from, raw.size(), raw.data());
        source.resize(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            source[i] = static_cast<float>(rfx::toConverterUnits(sm.format(), raw[i], bits));
        }
    }
    const auto at = [&](qint64 index) { return index >= from && index < to ? source[static_cast<size_t>(index - from)] : 0.0f; };
    std::vector<double> out(static_cast<size_t>(count));
    if (valid) {
        valid->assign(static_cast<size_t>(count), 0);
    }
    for (qint64 i = 0; i < count; ++i) {
        const double x = base.sourceAt(static_cast<double>(start + i));
        if (valid) {
            // (where the capture is: before it began and after it ended there is nothing, which is not a signal of zero)
            (*valid)[static_cast<size_t>(i)] = x >= 0.0 && x <= static_cast<double>(sm.words) - 1.0 ? 1 : 0;
        }
        const qint64 j = static_cast<qint64>(std::floor(x));
        out[static_cast<size_t>(i)] = cubic(at(j - 1), at(j), at(j + 1), at(j + 2), x - static_cast<double>(j));
    }
    return out;
}
// The speed of the disc over a capture from the fields pass 0 located: at the centre of each field, the line spacing over it against the
// nominal one (shorter lines are a faster disc), in ppm.
std::shared_ptr<std::vector<std::pair<double, double>>> speedFromFields(const std::vector<rfx::Field> &fields, double nominal)
{
    auto made = std::make_shared<std::vector<std::pair<double, double>>>();
    for (const rfx::Field &f : fields) {
        if (f.lines.size() < 20) {
            continue;
        }
        const double spacing = (f.lines.back() - f.lines.front()) / static_cast<double>(f.lines.size() - 1);
        made->emplace_back((f.lines.front() + f.lines.back()) / 2.0, (nominal / spacing - 1.0) * 1e6);
    }
    return made;
}
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
    bool stack = false;
    bool residue = false;
    bool combinedResidue = true;
};

// The stack of the aligned tracks shown (their mean), and the noise it removes: for the waveform, over the window, and for the spectrum, over
// the stretch round the bar (the part of it all of them have). The residue of a track is what it has more than the stack; the one of all of them
// is their spread round it (the standard deviation), drawn as a band, and its spectrum the mean of the spectra of the residues.
void makeTraces(const Request &q, AnalysisResult &r, const std::vector<std::vector<char>> &validity, const std::atomic<quint64> &current)
{
    // Only when every track shown has its time base: with some still being worked out, a stack of the others would be a stack of part of them.
    std::vector<size_t> part;
    for (size_t k = 0; k < q.tracks.size(); ++k) {
        if (!q.visible[k]) {
            continue;
        }
        if (!q.tracks[k].base || k >= r.wave.size() || r.wave[k].empty() || validity[k].size() != r.wave[k].size()) {
            return;
        }
        part.push_back(k);
    }
    if (part.empty()) {
        return;
    }
    const size_t n = static_cast<size_t>(q.windowLength);
    // At each sample the mean, the residues and their spread are of the tracks that have it (all of them but near the ends of a capture).
    std::vector<float> stack(n, 0.0F);
    std::vector<int> have(n, 0);
    for (const size_t k : part) {
        for (size_t i = 0; i < n; ++i) {
            if (validity[k][i]) {
                stack[i] += r.wave[k][i];
                ++have[i];
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        stack[i] = have[i] ? stack[i] / static_cast<float>(have[i]) : 0.0F;
    }
    std::vector<std::vector<float>> residues;
    for (const size_t k : part) {
        std::vector<float> res(n, 0.0F);
        for (size_t i = 0; i < n; ++i) {
            if (validity[k][i]) {
                res[i] = r.wave[k][i] - stack[i];
            }
        }
        residues.push_back(std::move(res));
    }
    std::vector<float> spread(n, 0.0F);
    for (size_t i = 0; i < n; ++i) {
        if (have[i] > 1) {
            double sum = 0.0;
            for (size_t j = 0; j < part.size(); ++j) {
                if (validity[part[j]][i]) {
                    sum += static_cast<double>(residues[j][i]) * residues[j][i];
                }
            }
            spread[i] = static_cast<float>(std::sqrt(sum / static_cast<double>(have[i] - 1)));
        }
    }
    // The spectra, over the stretch all of them have.
    const qint64 from = q.analysedCenter - q.analysedLength / 2;
    qint64 lo = from;
    qint64 hi = from + q.analysedLength;
    for (const size_t k : part) {
        const TimeBase &base = *q.tracks[k].base;
        const double words = static_cast<double>(q.tracks[k].reader->meta().sources[q.tracks[k].local].words);
        while (lo < hi && base.sourceAt(static_cast<double>(lo)) < 0.0) {
            lo += std::max<qint64>(1, (hi - lo) / 64);
        }
        while (hi > lo && base.sourceAt(static_cast<double>(hi)) > words) {
            hi -= std::max<qint64>(1, (hi - lo) / 64);
        }
    }
    std::vector<rfx::Spectrum> stackSpectrum(1);
    std::vector<rfx::Spectrum> residueSpectra;
    if (hi - lo >= kMinimumRange && current.load() == q.generation) {
        const rfx::SourceMeta &first = q.tracks[part.front()].reader->meta().sources[q.tracks[part.front()].local];
        const int ownBits = rfx::carriedBits(first.format(), first.shift);
        const double rate = first.rateHz > 0 ? first.rateHz : q.rateHz;
        std::vector<std::vector<double>> xs;
        for (const size_t k : part) {
            const rfx::SourceMeta &sm = q.tracks[k].reader->meta().sources[q.tracks[k].local];
            xs.push_back(readCorrected(*q.tracks[k].reader, q.tracks[k].local, sm, *q.tracks[k].base, lo, hi - lo, ownBits));
        }
        std::vector<double> mean(xs.front().size(), 0.0);
        for (const auto &x : xs) {
            for (size_t i = 0; i < mean.size(); ++i) {
                mean[i] += x[i] / static_cast<double>(xs.size());
            }
        }
        const double fullScale = std::ldexp(1.0, ownBits - 1);
        stackSpectrum[0] = rfx::analyseSpectrum(mean, static_cast<size_t>(q.fftSize), rate, fullScale);
        if (q.residue) {
            for (const auto &x : xs) {
                std::vector<double> res(x.size());
                for (size_t i = 0; i < res.size(); ++i) {
                    res[i] = x[i] - mean[i];
                }
                residueSpectra.push_back(rfx::analyseSpectrum(res, static_cast<size_t>(q.fftSize), rate, fullScale));
            }
        }
    }

    if (q.stack) {
        Trace t;
        t.label = QObject::tr("stack");
        t.wave = std::move(stack);
        t.spectrum = std::move(stackSpectrum[0]);
        r.traces.push_back(std::move(t));
    }
    if (q.residue) {
        if (q.combinedResidue) {
            Trace t;
            t.label = QObject::tr("residue");
            t.band = true;
            t.wave = std::move(spread);
            if (!residueSpectra.empty()) {
                // (the mean, bin by bin, of the spectra of the residues)
                t.spectrum = residueSpectra.front();
                for (size_t b = 0; b < t.spectrum.db.size(); ++b) {
                    double db = 0.0;
                    double power = 0.0;
                    for (const auto &s : residueSpectra) {
                        db += static_cast<double>(s.db[b]) / static_cast<double>(residueSpectra.size());
                        power += s.power[b] / static_cast<double>(residueSpectra.size());
                    }
                    t.spectrum.db[b] = static_cast<float>(db);
                    t.spectrum.power[b] = power;
                }
            }
            r.traces.push_back(std::move(t));
        } else {
            for (size_t j = 0; j < part.size(); ++j) {
                Trace t;
                t.track = static_cast<int>(part[j]);
                t.label = QObject::tr("residue %1").arg(part[j]);
                t.wave = std::move(residues[j]);
                if (j < residueSpectra.size()) {
                    t.spectrum = std::move(residueSpectra[j]);
                }
                r.traces.push_back(std::move(t));
            }
        }
    }
}
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
    std::vector<std::vector<char>> validity(tracks);

    // What is made for one track: its samples over the window, and its spectrum and statistics over the stretch.
    const auto work = [&](size_t k) {
        std::vector<int32_t> raw;
        rfx::TrackReader &reader = *q.tracks[k].reader;
        const size_t local = q.tracks[k].local;
        const rfx::SourceMeta &sm = reader.meta().sources[local];
        const int ownBits = rfx::carriedBits(sm.format(), sm.shift);

        if (q.tracks[k].base) {
            // The time base corrected: the window is of the corrected time, and the track is read where that time is.
            const std::vector<double> samples = readCorrected(reader, local, sm, *q.tracks[k].base, q.windowStart, q.windowLength, q.displayBits, &validity[k]);
            r.wave[k].assign(samples.begin(), samples.end());
        } else {
            raw.assign(static_cast<size_t>(q.windowLength), 0);
            reader.read(local, q.windowStart, raw.size(), raw.data());
            r.wave[k].resize(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                r.wave[k][i] = static_cast<float>(rfx::toConverterUnits(sm.format(), raw[i], q.displayBits));
            }
        }
        if (current.load() != q.generation) {
            return;
        }
        // Only what is in the track: a stretch that goes over its ends is cut, not
        // padded with zeros, which would be taken for signal. Too little left, and there
        // is nothing to say.
        const qint64 from = q.analysedCenter - q.analysedLength / 2;
        qint64 lo = std::max<qint64>(from, 0);
        qint64 hi = std::min<qint64>(from + q.analysedLength, static_cast<qint64>(sm.words));
        const TimeBase *base = q.tracks[k].base.get();
        if (base) {
            // (on the corrected time the stretch is cut where the capture ends: where its place in it is 0 and its length)
            lo = from;
            hi = from + q.analysedLength;
            const double words = static_cast<double>(sm.words);
            while (lo < hi && base->sourceAt(static_cast<double>(lo)) < 0.0) {
                lo += std::max<qint64>(1, (hi - lo) / 64);
            }
            while (hi > lo && base->sourceAt(static_cast<double>(hi)) > words) {
                hi -= std::max<qint64>(1, (hi - lo) / 64);
            }
        }
        if (hi - lo < kMinimumRange) {
            r.pastEnd[k] = true;
            return;
        }
        std::vector<double> x;
        if (base) {
            x = readCorrected(reader, local, sm, *base, lo, hi - lo, ownBits);
        } else {
            raw.assign(static_cast<size_t>(hi - lo), 0);
            reader.read(local, lo, raw.size(), raw.data());
            x.resize(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                x[i] = rfx::toConverterUnits(sm.format(), raw[i], ownBits);
            }
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
    if ((q.stack || q.residue) && failure.isEmpty() && current.load() == q.generation) {
        makeTraces(q, r, validity, current);
    }
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
    // The TBC curves an archive keeps are there to be shown at once.
    {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        m_rfCurves.assign(m_tracks.size(), nullptr);
        m_rfSaved.assign(m_tracks.size(), false);
        m_rfData.assign(m_tracks.size(), nullptr);
        m_timebases.assign(m_tracks.size(), nullptr);
        m_rfStarted.assign(m_tracks.size(), false);
        for (size_t k = 0; k < m_tracks.size(); ++k) {
            const rfx::SavedPass0 *saved = m_sources[static_cast<size_t>(m_tracks[k].source)].reader->pass0(m_tracks[k].local);
            if (saved) {
                const rfx::VideoSystem system = saved->pal ? rfx::VideoSystem::Pal : rfx::VideoSystem::Ntsc;
                m_rfCurves[k] = speedFromFields(saved->fields, rfx::nominalLineSamples(saved->rateHz, system));
                m_rfData[k] = std::make_shared<RfData>(RfData{saved->fields, saved->pal, saved->rateHz});
                m_rfSaved[k] = true;
                m_rfStarted[k] = true;
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
    return true;
}

void Session::close()
{
    if (m_sources.empty()) {
        return;
    }
    ++m_generation;
    ++m_openId;
    // The speed from the RF reads the captures: stopped, and waited for, before they go.
    m_speedStop = true;
    m_speedPool.waitForDone();
    m_pool.waitForDone();
    m_overviewPool.waitForDone();
    {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        m_overview.clear();
        m_rfCurves.clear();
        m_rfData.clear();
        m_timebases.clear();
        m_rfStarted.clear();
        m_rfPct.reset();
        m_rfPctCount = 0;
        m_rfOn.clear();
        m_aligned.clear();
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
    m_alignPlaced = false;
    placeAligned();   // (the tracks shown are not the same: neither is the place where all of them are)
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

size_t Session::localTrack(int k) const
{
    return m_tracks[static_cast<size_t>(k)].local;
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
    q.stack = m_stack;
    q.residue = m_residue;
    q.combinedResidue = m_combinedResidue;
    for (size_t k = 0; k < m_tracks.size(); ++k) {
        const TrackRef &t = m_tracks[k];
        q.tracks.push_back({m_sources[static_cast<size_t>(t.source)].reader.get(), t.local, aligned(static_cast<int>(k)) ? timebaseOf(static_cast<int>(k)) : nullptr});
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

bool Session::rfSpeed(int k) const
{
    return k >= 0 && static_cast<size_t>(k) < m_rfOn.size() && m_rfOn[static_cast<size_t>(k)];
}

void Session::computeCurve(int k)
{
    ActionScope scope(this);
    if (k >= 0 && k < trackCount() && !curveReady(k) && allowed(k)) {
        startRfSpeed(k);
        emit rfSpeedProgress();   // (the list of tracks shows that it waits)
    }
}

void Session::setRfSpeed(int k, bool on)
{
    ActionScope scope(this);
    if (k < 0 || k >= trackCount() || on == rfSpeed(k)) {
        return;
    }
    if (on && !allowed(k)) {
        emit rfSpeedChanged();   // (the boxes go back to what is)
        return;
    }
    m_rfOn.resize(m_tracks.size(), false);
    m_rfOn[static_cast<size_t>(k)] = on;
    if (on) {
        startRfSpeed(k);
    }
    emit rfSpeedChanged();
}

std::shared_ptr<const std::vector<std::pair<double, double>>> Session::curveOf(int k) const
{
    std::lock_guard<std::mutex> lock(m_overviewMutex);
    if (k >= 0 && static_cast<size_t>(k) < m_rfCurves.size()) {
        return m_rfCurves[static_cast<size_t>(k)];
    }
    return nullptr;
}

bool Session::curveReady(int k) const
{
    const auto curve = curveOf(k);
    return curve && !curve->empty();
}

bool Session::canSaveSpeed() const
{
    return std::any_of(m_sources.begin(), m_sources.end(), [](const Loaded &l) { return l.reader->keepsSpeed(); });
}

int Session::unsavedCurves() const
{
    int n = 0;
    for (size_t k = 0; k < m_tracks.size(); ++k) {
        const bool saved = k < m_rfSaved.size() && m_rfSaved[k];
        if (!saved && curveReady(static_cast<int>(k)) && m_sources[static_cast<size_t>(m_tracks[k].source)].reader->keepsSpeed()) {
            ++n;
        }
    }
    return n;
}

int Session::saveSpeedCurves(QString *error)
{
    int kept = 0;
    try {
        for (size_t s = 0; s < m_sources.size(); ++s) {
            if (!m_sources[s].reader->keepsSpeed()) {
                continue;
            }
            std::map<size_t, rfx::SavedPass0> curves;
            for (size_t k = 0; k < m_tracks.size(); ++k) {
                if (static_cast<size_t>(m_tracks[k].source) == s && curveReady(static_cast<int>(k))) {
                    std::shared_ptr<const RfData> data;
                    {
                        std::lock_guard<std::mutex> lock(m_overviewMutex);
                        data = k < m_rfData.size() ? m_rfData[k] : nullptr;
                    }
                    if (data) {
                        curves[m_tracks[k].local] = rfx::SavedPass0{data->pal, data->rate, data->fields};
                    }
                }
            }
            if (!curves.empty()) {
                rfx::savePass0(m_sources[s].reader->path(), curves);
                kept += static_cast<int>(curves.size());
                std::lock_guard<std::mutex> lock(m_overviewMutex);
                for (size_t k = 0; k < m_tracks.size() && k < m_rfSaved.size(); ++k) {
                    if (static_cast<size_t>(m_tracks[k].source) == s && curves.count(m_tracks[k].local)) {
                        m_rfSaved[k] = true;
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return -1;
    }
    return kept;
}

int Session::rfProgressOf(int k) const
{
    // (-1: nothing is being done for it, -2: it waits, else the percent: set by whatever asked for the work)
    return static_cast<size_t>(k) < m_rfPctCount ? m_rfPct[static_cast<size_t>(k)].load() : -1;
}

int Session::rfWorkingTracks() const
{
    int n = 0;
    for (size_t k = 0; k < m_rfPctCount; ++k) {
        n += m_rfPct[k].load() >= 0 ? 1 : 0;
    }
    return n;
}

int Session::rfMeanPercent() const
{
    int n = 0;
    int sum = 0;
    for (size_t k = 0; k < m_rfPctCount; ++k) {
        const int pct = m_rfPct[k].load();
        if (pct >= 0) {
            ++n;
            sum += pct;
        }
    }
    return n ? sum / n : 0;
}

// The time base of a track, once what it is made from is there: its own lines, and (for a track other than the first) those of the first,
// which give the common axis. Null until then.
std::shared_ptr<const TimeBase> Session::timebaseOf(int k)
{
    std::lock_guard<std::mutex> lock(m_overviewMutex);
    if (k < 0 || static_cast<size_t>(k) >= m_timebases.size()) {
        return nullptr;
    }
    if (m_timebases[static_cast<size_t>(k)]) {
        return m_timebases[static_cast<size_t>(k)];
    }
    const auto own = m_rfData[static_cast<size_t>(k)];
    const auto anchor = m_rfData.empty() ? nullptr : m_rfData[0];
    if (!own || (k != 0 && !anchor)) {
        return nullptr;
    }
    m_timebases[static_cast<size_t>(k)] = TimeBase::make(own->fields, own->pal, own->rate, k == 0 ? nullptr : &anchor->fields);
    return m_timebases[static_cast<size_t>(k)];
}

void Session::setStackShown(bool on)
{
    if (on != m_stack) {
        m_stack = on;
        emit changed();
        refresh();
    }
}

void Session::setResidueShown(bool on)
{
    if (on != m_residue) {
        m_residue = on;
        emit changed();
        refresh();
    }
}

void Session::setCombinedResidue(bool on)
{
    if (on != m_combinedResidue) {
        m_combinedResidue = on;
        emit changed();
        refresh();
    }
}

bool Session::aligned(int k) const
{
    return k >= 0 && static_cast<size_t>(k) < m_aligned.size() && m_aligned[static_cast<size_t>(k)];
}

// The time base of a track corrected by its speed (the same curve as the graph of the speed): asking for it asks for the curve.
void Session::setAligned(int k, bool on)
{
    ActionScope scope(this);
    if (k < 0 || k >= trackCount() || on == aligned(k)) {
        return;
    }
    if (on && !allowed(k)) {
        emit rfSpeedChanged();
        return;
    }
    m_aligned.resize(m_tracks.size(), false);
    m_aligned[static_cast<size_t>(k)] = on;
    m_alignPlaced = false;
    if (on) {
        setRfSpeed(k, true);
        if (k != 0 && allowed(0)) {
            startRfSpeed(0);   // (the first track gives the axis the others are lined up on)
        }
    }
    emit rfSpeedChanged();
    placeAligned();
    refresh();
}

// On the corrected time the captures do not all start at the same place (one that began later on the disc has nothing before it): once every track
// shown and aligned has its time base, a window that lies before the place where all of them are is moved to it, so that they are all in view.
void Session::placeAligned()
{
    if (m_alignPlaced) {
        return;
    }
    double common = 0.0;
    bool any = false;
    for (int k = 0; k < trackCount(); ++k) {
        if (!aligned(k) || !visible(k)) {
            continue;
        }
        const auto base = timebaseOf(k);
        if (!base) {
            return;   // (not all of them are ready: later)
        }
        common = std::max(common, base->tau.front());
        any = true;
    }
    if (!any) {
        return;
    }
    m_alignPlaced = true;
    if (static_cast<double>(m_windowStart) < common) {
        setWindowStart(static_cast<qint64>(std::ceil(common)));
    }
    // (the spectrum and the statistics are of the stretch round the bar: it has to be where the captures are too)
    if (static_cast<double>(m_cursor) < common) {
        setCursor(m_windowStart + m_windowLength / 2, false);
    }
}

bool Session::hasSpeed(int k) const
{
    if (rfSpeed(k)) {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        return static_cast<size_t>(k) < m_rfCurves.size() && m_rfCurves[static_cast<size_t>(k)] != nullptr;
    }
    return lagTable(k) != nullptr;
}

// The lines of a track are located from its RF, on a thread of its own (the filters use all the
// processors); what is made stays until the files change.
void Session::startRfSpeed(int only)
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
        if (m_rfPctCount != m_tracks.size()) {
            m_rfPct = std::make_unique<std::atomic<int>[]>(m_tracks.size());
            m_rfPctCount = m_tracks.size();
            for (size_t k = 0; k < m_rfPctCount; ++k) {
                m_rfPct[k] = -1;
            }
        }
        for (size_t k = 0; k < m_tracks.size(); ++k) {
            if (m_rfStarted[k] || static_cast<int>(k) != only) {
                continue;
            }
            m_rfStarted[k] = true;
            m_rfPct[k] = -2;   // (waiting for its turn)
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
    // The tracks asked for are worked on together: they share what is decoded of the tracks they are coded against (each by itself
    // would decode them all again), and the processors are shared between them.
    m_speedPool.setMaxThreadCount(8);
    m_speedPool.start(QRunnable::create([this, jobs, id]() {
        // A quick look first, at the start of each track: which system its lines are of. When the standard said for a track seems wrong, the
        // work waits (with the others that started with it) for the person to answer, and then goes on with the right system.
        const auto stale = [this, id] { return m_openId.load() != id || m_speedStop.load(); };
        std::map<int, rfx::VideoSystem> firstTry;
        ++m_detecting;
        bool asks = false;
        {
            std::vector<std::pair<int, QString>> doubts;
            for (const Job &job : jobs) {
                if (stale()) {
                    break;
                }
                try {
                    const std::string &said = job.reader->meta().sources[job.local].standard;
                    if (const auto system = rfx::detectSystem(*job.reader, job.local, job.rate, said, &m_speedStop)) {
                        firstTry[job.track] = *system;
                        if (const auto suggestion = rfx::doubtedStandard(said, *system == rfx::VideoSystem::Pal)) {
                            doubts.push_back({job.track, QString::fromStdString(*suggestion)});
                        }
                    }
                } catch (const std::exception &) {
                    // (the work itself says what is wrong with the track)
                }
            }
            std::lock_guard<std::mutex> lock(m_doubtMutex);
            m_standardDoubts.insert(m_standardDoubts.end(), doubts.begin(), doubts.end());
            // The last one to have looked asks, if anything seems wrong; the others wait for it.
            if (--m_detecting == 0 && !m_standardDoubts.empty() && !stale()) {
                m_asking = true;
                asks = true;
            }
        }
        if (asks) {
            QMetaObject::invokeMethod(this, [this, id] {
                if (m_openId.load() == id) {
                    emit standardsDoubtful();
                }
            }, Qt::QueuedConnection);
        }
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m_doubtMutex);
                if (m_detecting.load() == 0 && !m_asking) {
                    break;
                }
            }
            if (stale()) {
                break;
            }
            QThread::msleep(25);
        }
        for (const Job &job : jobs) {
            std::shared_ptr<SpeedCurve> curve;
            std::shared_ptr<const RfData> data;
            if (!stale()) {
                try {
                    // The system the standard of the track says first (NTSC when it says nothing); a PAL capture gives no field as NTSC.
                    m_rfPct[static_cast<size_t>(job.track)] = 0;
                    QMetaObject::invokeMethod(this, [this] { emit rfSpeedProgress(); }, Qt::QueuedConnection);
                    std::array<rfx::VideoSystem, 2> order = rfx::systemsToTry(job.reader->meta().sources[job.local].standard);
                    if (const auto seen = firstTry.find(job.track); seen != firstTry.end() && order[0] != seen->second) {
                        std::swap(order[0], order[1]);
                    }
                    for (const rfx::VideoSystem system : order) {
                        const std::vector<rfx::Field> fields = rfx::locateFields(*job.reader, job.local, job.rate, system, 0, &m_speedStop, [this, job](double done) {
                            const int percent = std::clamp(static_cast<int>(done * 100.0), 0, 100);
                            if (percent != m_rfPct[static_cast<size_t>(job.track)].exchange(percent)) {
                                QMetaObject::invokeMethod(this, [this] { emit rfSpeedProgress(); }, Qt::QueuedConnection);
                            }
                        });
                        const auto made = speedFromFields(fields, rfx::nominalLineSamples(job.rate, system));
                        if (!made->empty()) {
                            curve = made;
                            data = std::make_shared<RfData>(RfData{fields, system == rfx::VideoSystem::Pal, job.rate});
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
                    m_rfSaved.resize(m_rfCurves.size(), false);
                    m_rfSaved[static_cast<size_t>(job.track)] = false;
                    m_rfData.resize(m_rfCurves.size());
                    m_rfData[static_cast<size_t>(job.track)] = data;
                    m_timebases.assign(m_rfCurves.size(), nullptr);   // (the axis of the others may come from this one)
                }
            }
            m_rfPct[static_cast<size_t>(job.track)] = -1;
            --m_rfBusy;
            QMetaObject::invokeMethod(this, [this, id] {
                if (m_openId.load() == id) {
                    emit rfSpeedChanged();
                    if (std::any_of(m_aligned.begin(), m_aligned.end(), [](bool on) { return on; })) {
                        placeAligned();
                        refresh();   // (the waveform now has its curve to be aligned by)
                    }
                }
            }, Qt::QueuedConnection);
        }
    }));
}

bool Session::replaceFile(const QString &path, const QString &replacement, QString *error)
{
    std::vector<size_t> mine;
    for (size_t i = 0; i < m_sources.size(); ++i) {
        if (m_sources[i].reader && m_sources[i].shownAs == path) {
            mine.push_back(i);
        }
    }
    const auto fail = [&](const QString &text) {
        QFile::remove(replacement);
        if (error) {
            *error = text;
        }
        return false;
    };
    if (mine.empty()) {
        return fail(tr("%1 is not open").arg(QDir::toNativeSeparators(path)));
    }
    if (m_rfBusy.load() > 0) {
        return fail(tr("A TBC curve is being worked out: wait for it to be done."));
    }
    // What reads the files is stopped and waited for, then the file is let go.
    ++m_openId;
    m_speedStop = true;
    m_speedPool.waitForDone();
    m_pool.waitForDone();
    m_overviewPool.waitForDone();
    for (const size_t i : mine) {
        m_sources[i].reader.reset();
    }
    QString failure;
    if (QFile::exists(path) && !QFile::remove(path)) {
        failure = tr("%1 cannot be replaced: is it open in another program?").arg(QDir::toNativeSeparators(path));
    } else if (!QFile::rename(replacement, path)) {
        failure = tr("%1 cannot be put in place of the file").arg(QDir::toNativeSeparators(replacement));
    }
    if (!failure.isEmpty()) {
        QFile::remove(replacement);
    }
    bool readable = true;
    for (const size_t i : mine) {
        try {
            m_sources[i].reader = std::make_unique<rfx::TrackReader>(path);
        } catch (const std::exception &e) {
            readable = false;
            failure += (failure.isEmpty() ? QString() : QStringLiteral("\n")) + QString::fromUtf8(e.what());
        }
    }
    if (!readable) {
        m_speedStop = false;
        // (nothing is left to show of a file that cannot be read)
        for (const size_t i : mine) {
            m_sources[i].reader.reset();
        }
        m_sources.clear();
        close();
        if (error) {
            *error = failure;
        }
        return false;
    }
    m_speedStop = false;
    ++m_generation;
    startOverview();
    refresh();
    emit tracksChanged();
    emit changed();
    if (!failure.isEmpty()) {
        if (error) {
            *error = failure;
        }
        return false;
    }
    return true;
}

std::string Session::mediumOf(int k) const
{
    if (k < 0 || k >= trackCount()) {
        return std::string();
    }
    return m_sources[static_cast<size_t>(m_tracks[static_cast<size_t>(k)].source)].reader->meta().medium;
}

std::vector<std::pair<int, QString>> Session::takeStandardDoubts()
{
    std::lock_guard<std::mutex> lock(m_doubtMutex);
    std::vector<std::pair<int, QString>> out;
    out.swap(m_standardDoubts);
    return out;
}

void Session::resumeCurves()
{
    std::lock_guard<std::mutex> lock(m_doubtMutex);
    m_asking = false;
}

std::vector<int> Session::abortCurves()
{
    m_speedStop = true;
    m_speedPool.waitForDone();
    m_speedStop = false;
    std::vector<int> unfinished;
    {
        std::lock_guard<std::mutex> lock(m_overviewMutex);
        for (size_t k = 0; k < m_rfStarted.size() && k < m_rfCurves.size(); ++k) {
            if (m_rfStarted[k] && !m_rfCurves[k]) {
                m_rfStarted[k] = false;
                unfinished.push_back(static_cast<int>(k));
            }
            if (m_rfPct && k < m_rfPctCount) {
                m_rfPct[k] = -1;
            }
        }
    }
    m_rfBusy = 0;
    m_detecting = 0;
    {
        std::lock_guard<std::mutex> lock(m_doubtMutex);
        m_asking = false;
        m_standardDoubts.clear();
    }
    emit rfSpeedChanged();
    emit rfSpeedProgress();
    return unfinished;
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
    // The mean over the same stretch as the stored speed is read over (speedWindow): the discs speed varies from one field to the next
    // by far more than over a few hundredths of the capture, and the two ways of getting the speed must show the same quantity.
    const double half = static_cast<double>(speedWindow(k)) / 2.0;
    const auto byPlace = [](const std::pair<double, double> &p, double v) { return p.first < v; };
    const auto lo = std::lower_bound(curve->begin(), curve->end(), x - half, byPlace);
    const auto hi = std::lower_bound(curve->begin(), curve->end(), x + half, byPlace);
    if (hi - lo >= 2) {
        double sum = 0.0;
        for (auto it = lo; it != hi; ++it) {
            sum += it->second;
        }
        return sum / static_cast<double>(hi - lo);
    }
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
    if (rfSpeed(k)) {
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
