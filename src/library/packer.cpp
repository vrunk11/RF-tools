/******************************************************************************
 * packer.cpp
 * rfx library - packing captures into an .rfx archive
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "packer.h"
#include "rfsync.h"
#include "trackreader.h"

#include "packer_internal.h"

#include "alignment.h"
#include "capture.h"
#include "ebml.h"
#include "error.h"
#include "gpupipeline.h"
#include "lagtable.h"
#include "metadata.h"
#include "order.h"
#include "parallel.h"
#include "posfile.h"
#include "predictor.h"
#include "refwindow.h"
#include "sampleformat.h"
#include "segment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

namespace rfx {

namespace {

// Windows (of kChooseWindow samples, spread through the target) on which the
// references of a capture are compared.
constexpr size_t kChooseWindows = 4;
constexpr size_t kChooseWindow = 131072;
// A reference's weight falls by half for each 1 / kChooseSteep bit its cost alone
// is above the best one's.
constexpr double kChooseSteep = 8.0;
// References weighing less than this (of the best's 1) are dropped, with their
// lag tables.
constexpr float kChooseFloor = 0.2f;
// The writer's limit on the references that get channels of their own is the
// level's `ntop`; with fewer references than this the plain mean is kept.
constexpr size_t kMinReferencesForChannels = 3;
// Tracks packed at once on a GPU: the device works on one while the processor codes
// another (each holds a chunk of up to a gigabyte).
constexpr unsigned kGpuTracksAtOnce = 2;

// Samples [s0, s0 + n) of track `k`, shifted and with the mean taken off.
std::vector<int32_t> trackSamples(PosFile &file, const SourceMeta &sm, int64_t s0, size_t n)
{
    std::vector<int32_t> y(n);
    loadSamples(sm.format(), file, sm.words, s0, y.data(), n);
    for (int32_t &v : y) {
        v = (v >> sm.shift) - sm.mean;
    }
    return y;
}

// Cost in bits per sample of the samples `y` with the window `window`.
double windowCost(const std::vector<int32_t> &y, const Preset &pre, const std::vector<float> &window)
{
    const std::vector<int32_t> residuals = predictionErrors(y, pre, true, window.data(), window.size() / pre.channels());
    return static_cast<double>(encodeResiduals(residuals).size()) * 8.0 / static_cast<double>(y.size());
}

std::vector<size_t> chooseStarts(const SourceMeta &sm, size_t win)
{
    std::vector<size_t> starts;
    for (size_t w = 0; w < kChooseWindows; ++w) {
        starts.push_back(((static_cast<size_t>(sm.words) - win) * (2 * w + 1)) / (2 * kChooseWindows));
    }
    return starts;
}

// The references of capture `k` and their weights. The mean of every earlier
// capture treats a good reference (another capture of the same disc, which shares
// the disc's own noise) like a poor one (another disc); each is weighed by how
// well it predicts the capture alone.
void chooseRefs(const ReferenceWindows &windows, const std::vector<PosFile *> &files, const ArchiveMeta &meta, size_t k,
                unsigned threads, std::vector<size_t> &refs, std::vector<float> &weights)
{
    const SourceMeta &sm = meta.sources[k];
    if (sm.refs.size() < 2) {
        refs = sm.refs;
        weights.assign(sm.refs.size(), 1.0f);
        return;
    }
    const Preset pre = sm.preset();
    const size_t win = std::min<size_t>(kChooseWindow, static_cast<size_t>(sm.words));
    const std::vector<size_t> starts = chooseStarts(sm, win);
    std::vector<double> costs(sm.refs.size() * starts.size());
    parallelFor(costs.size(), threads, [&](size_t job) {
        const size_t r = job / starts.size();
        const size_t s0 = starts[job % starts.size()];
        const std::vector<int32_t> y = trackSamples(*files[k], sm, static_cast<int64_t>(s0), win);
        const std::vector<float> w = windows.windowWith(k, static_cast<int64_t>(s0), win, {{sm.refs[r], 1.0f}});
        costs[job] = windowCost(y, pre, w);
    });
    auto cost = [&](size_t r) {
        double sum = 0;
        for (size_t i = 0; i < starts.size(); ++i) {
            sum += costs[r * starts.size() + i];
        }
        return sum / static_cast<double>(starts.size());
    };
    double best = std::numeric_limits<double>::max();
    for (size_t r = 0; r < sm.refs.size(); ++r) {
        best = std::min(best, cost(r));
    }
    std::vector<float> raw(sm.refs.size());
    for (size_t r = 0; r < raw.size(); ++r) {
        raw[r] = static_cast<float>(std::pow(2.0, -kChooseSteep * (cost(r) - best)));
    }
    std::vector<size_t> keep;
    float sum = 0.0f;
    for (size_t r = 0; r < raw.size(); ++r) {
        if (raw[r] >= kChooseFloor) {
            keep.push_back(r);
            sum += raw[r];
        }
    }
    refs.clear();
    weights.clear();
    for (const size_t r : keep) {
        refs.push_back(sm.refs[r]);
        weights.push_back(raw[r] / sum);
    }
}

// Cost, in bits per sample, of coding the trial windows of capture `k` with the
// predictor its metadata describes.
double trialCost(const ReferenceWindows &windows, const std::vector<PosFile *> &files, const ArchiveMeta &meta, size_t k,
                 unsigned threads)
{
    const SourceMeta &sm = meta.sources[k];
    const Preset pre = sm.preset();
    const size_t win = std::min<size_t>(kChooseWindow, static_cast<size_t>(sm.words));
    const std::vector<size_t> starts = chooseStarts(sm, win);
    std::vector<double> costs(starts.size());
    parallelFor(costs.size(), threads, [&](size_t i) {
        const std::vector<int32_t> y = trackSamples(*files[k], sm, static_cast<int64_t>(starts[i]), win);
        costs[i] = windowCost(y, pre, windows.window(k, static_cast<int64_t>(starts[i]), win));
    });
    double sum = 0;
    for (const double c : costs) {
        sum += c;
    }
    return sum / static_cast<double>(costs.size());
}

std::string perCent(double part, double whole)
{
    return QString::asprintf("%.1f", 100.0 * part / std::max(whole, 1.0)).toStdString();
}

void writeAll(QFile &out, const std::vector<uint8_t> &bytes)
{
    if (out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size())) {
        throw Error("cannot write " + out.fileName().toStdString() + ": " + out.errorString().toStdString());
    }
}

} // namespace

void writeArchiveHead(QFile &out, const ArchiveMeta &meta, const LagTables &tables)
{
    const uint64_t n = meta.sources.size();
    writeAll(out, ebml::fileHead());
    writeAll(out, ebml::info());
    std::vector<ebml::Bytes> entries;
    for (size_t k = 0; k < meta.sources.size(); ++k) {
        entries.push_back(ebml::trackEntry(k + 1, kCodecPred, meta.sources[k].name));
    }
    entries.push_back(ebml::trackEntry(n + 1, kCodecMeta, "meta"));
    entries.push_back(ebml::trackEntry(n + 2, kCodecMap, "map"));
    writeAll(out, ebml::tracks(entries));
    writeAll(out, ebml::cluster(0, n + 1, serializeMeta(meta)));
    uint64_t rank = 0;
    for (const auto &t : tables) {
        std::vector<uint8_t> b;
        putVarint(b, t.first.first);
        putVarint(b, t.first.second);
        const std::vector<uint8_t> coded = t.second.encode();
        b.insert(b.end(), coded.begin(), coded.end());
        writeAll(out, ebml::cluster(rank++, n + 2, b));
    }
}

uint64_t copyBlocks(QFile &out, Archive &archive, size_t src, size_t k)
{
    std::vector<std::pair<uint64_t, BlockSpan>> blocks(archive.blocks(src).begin(), archive.blocks(src).end());
    std::sort(blocks.begin(), blocks.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    uint64_t total = 0;
    for (const auto &b : blocks) {
        std::vector<uint8_t> bytes(static_cast<size_t>(b.second.length));
        if (!bytes.empty()) {
            archive.file().readAt(bytes.data(), bytes.size(), b.second.offset);
        }
        total += b.second.length;
        writeAll(out, ebml::cluster(b.first, k + 1, bytes));
    }
    return total;
}

std::unique_ptr<PreparedPack> prepareCaptures(const std::vector<QString> &given, const std::vector<PackSpec> &specs,
                                              const LagTables &known, const QString &output, const PackOptions &o)
{
    if (given.empty()) {
        throw Error("no input");
    }
    if (specs.size() != given.size()) {
        throw Error("every input needs its spec");
    }
    const unsigned threads = o.threads != 0 ? o.threads : defaultThreads();
    auto prepared = std::make_unique<PreparedPack>();
    const auto phaseStart = std::chrono::steady_clock::now();
    auto phase = [&phaseStart](const char *what) {
        if (!qEnvironmentVariableIsEmpty("RFX_TIMING")) {
            std::fprintf(stderr, "  [%.2f s] %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - phaseStart).count(), what);
        }
    };
    const bool edit = std::any_of(specs.begin(), specs.end(), [](const PackSpec &s) { return s.carry.has_value(); });
    // Compressed captures (FLAC, Ogg-FLAC) are decoded to raw 16-bit files first:
    // the references are read at arbitrary positions.
    const DecodedInputs decoded = decodeCompressed(given, output, o.format);
    prepared->workDir = decoded.workDir;
    std::vector<QString> inputs = decoded.paths;
    std::vector<QString> origs = given;
    // The level of coding of each track: the one of the options, unless the track was given its own.
    std::vector<Preset> presets(given.size());
    for (size_t k = 0; k < given.size(); ++k) {
        const int level = specs[k].level.value_or(o.level);
        if (!Preset::level(level, presets[k])) {
            throw Error("level " + std::to_string(level) + " is not 1 to 4");
        }
    }
    std::vector<std::string> names;
    for (size_t k = 0; k < inputs.size(); ++k) {
        const std::string n = specs[k].carry ? specs[k].carry->name : QFileInfo(inputs[k]).fileName().toStdString();
        if (n.empty()) {
            throw Error("input has no file name");
        }
        if (std::find(names.begin(), names.end(), n) != names.end()) {
            throw Error("two captures are named " + n + ": the archive keeps file names only");
        }
        names.push_back(n);
    }
    std::vector<SampleFormat> fmts;
    for (size_t k = 0; k < inputs.size(); ++k) {
        fmts.push_back(specs[k].carry ? specs[k].carry->format() : inputFormat(inputs[k], o.format));
    }
    const size_t toScan = static_cast<size_t>(std::count_if(specs.begin(), specs.end(), [](const PackSpec &s) { return !s.carry; }));
    if (toScan > 0) {
        std::fprintf(stderr, "hashing and scanning %zu files\n", toScan);
    }
    std::vector<CaptureStats> stats(inputs.size());
    // The files are opened here, not in the workers: opening a QFile in a thread is slow.
    std::vector<std::unique_ptr<PosFile>> scanned(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (!specs[i].carry) {
            scanned[i] = std::make_unique<PosFile>(inputs[i], PosFile::Mode::Read);
        }
    }
    parallelFor(inputs.size(), threads, [&](size_t i) {
        if (specs[i].carry) {
            const SourceMeta &c = *specs[i].carry;
            stats[i].size = c.size;
            stats[i].sha = c.sha256;
            stats[i].shift = c.shift;
            stats[i].mean = c.mean;
            stats[i].sampleSha = c.sampleSha256;
        } else {
            stats[i] = captureStats(*scanned[i], fmts[i]);
        }
    });
    phase("captures hashed and scanned");

    // Where the lines of every capture are, from ld-decode's pass 0 (in the order
    // given), then the order to code them in.
    bool needTables = false;
    for (size_t k = 1; k < specs.size(); ++k) {
        needTables = needTables || !specs[k].settled();
    }
    std::vector<std::vector<Field>> fields;
    if (inputs.size() > 1 && needTables) {
        fields.resize(inputs.size());
        if (o.pass0Dir || o.externalPass0) {
            // Dumps already made, or made by ld-decode.
            QString dir;
            if (o.pass0Dir) {
                dir = *o.pass0Dir;
            } else {
                dir = withExtension(output, QStringLiteral("pass0"));
                std::fprintf(stderr, "locating the lines of every capture (ld-decode pass 0)\n");
                if (std::any_of(fmts.begin(), fmts.end(), [&](SampleFormat f) { return f != fmts[0]; })) {
                    throw Error("the captures are not all in one format: the pass 0 of ld-decode takes one");
                }
                std::optional<double> hz;
                if (specs[0].carry && specs[0].carry->rateHz > 0) {
                    hz = specs[0].carry->rateHz;
                } else {
                    hz = sidecarRateHz(findSidecars(given[0]));
                }
                runPass0(o.ldDecode, inputs, hz ? *hz / 1e6 : o.rateMhz, formatName(fmts[0]), dir);
            }
            phase("pass 0 directory ready");
            for (size_t i = 0; i < inputs.size(); ++i) {
                fields[i] = parsePass0(QDir(dir).filePath(QStringLiteral("pass0_%1.txt").arg(static_cast<qulonglong>(i))));
            }
            phase("pass 0 parsed");
        } else {
            // The lines are located here, from the RF of each capture, with the same results as ld-decode's pass 0.
            std::fprintf(stderr, "locating the lines of every capture (pass 0)\n");
            std::optional<double> hz;
            if (specs[0].carry && specs[0].carry->rateHz > 0) {
                hz = specs[0].carry->rateHz;
            } else {
                hz = sidecarRateHz(findSidecars(given[0]));
            }
            const double fs = hz ? *hz : o.rateMhz * 1e6;
            for (size_t i = 0; i < inputs.size(); ++i) {
                CaptureSpec capture;
                capture.format = fmts[i];
                capture.rateHz = fs;
                capture.bits = 16;
                TrackReader reader(inputs[i], capture);
                int shown = -1;
                fields[i] = locateFields(reader, 0, fs, o.pal ? VideoSystem::Pal : VideoSystem::Ntsc, o.threads, nullptr, [&](double part) {
                    const int percent = static_cast<int>(part * 100.0);
                    if (percent != shown) {
                        shown = percent;
                        std::fprintf(stderr, "pass 0 [%zu/%zu] %s: %d%%\r", i + 1, inputs.size(), names[i].c_str(), percent);
                    }
                });
                std::fprintf(stderr, "pass 0 [%zu/%zu] %s: 100%%\n", i + 1, inputs.size(), names[i].c_str());
            }
            phase("pass 0 done");
        }
    }
    phase("pass 0 read, order planned or not yet");
    std::vector<std::optional<double>> noise(inputs.size());
    for (size_t k = 0; k < inputs.size(); ++k) {
        if (specs[k].carry) {
            noise[k] = specs[k].carry->noise;
        }
    }
    std::vector<size_t> ord(inputs.size());
    for (size_t i = 0; i < ord.size(); ++i) {
        ord[i] = i;
    }
    if (inputs.size() > 1 && o.order == OrderMode::Auto && !edit) {
        const Plan plan = planOrder(inputs, fmts, fields, threads);
        ord = plan.order;
        noise = plan.noise;
        bool reordered = false;
        for (size_t k = 0; k < ord.size(); ++k) {
            reordered = reordered || ord[k] != k;
        }
        if (reordered) {
            std::fprintf(stderr, "coding order, cleanest first (noise against signal, smaller is cleaner):\n");
            for (size_t k = 0; k < ord.size(); ++k) {
                const size_t i = ord[k];
                if (noise[i]) {
                    std::fprintf(stderr, "  %2zu: %s (was %zu, %.3f)\n", k, names[i].c_str(), i, *noise[i]);
                } else {
                    std::fprintf(stderr, "  %2zu: %s (was %zu)\n", k, names[i].c_str(), i);
                }
            }
        }
    }
    // From here on, everything is in coding order; ord[k] is the place on the
    // command line.
    auto reorder = [&ord](auto &v) {
        auto copy = v;
        for (size_t k = 0; k < ord.size(); ++k) {
            v[k] = std::move(copy[ord[k]]);
        }
    };
    std::vector<PackSpec> places = specs;
    reorder(inputs);
    reorder(origs);
    reorder(names);
    reorder(stats);
    reorder(fmts);
    reorder(noise);
    reorder(places);
    if (!fields.empty()) {
        reorder(fields);
    }
    LagTables &tables = prepared->tables;
    for (size_t k = 1; k < inputs.size(); ++k) {
        for (size_t j = 0; j < k; ++j) {
            if (places[k].settled()) {
                const auto it = known.find({k, j});
                if (it != known.end()) {
                    tables[{k, j}] = it->second;
                }
                continue;
            }
            std::optional<LagTable> t = buildLagTable(fields[k], fields[j], formatSamples(fmts[k], stats[k].size), kHysteresisFine);
            if (t) {
                tables[{k, j}] = std::move(*t);
            } else {
                std::fprintf(stderr, "%s and %s share no frame: not used as a reference\n", names[k].c_str(), names[j].c_str());
            }
        }
    }

    phase("lag tables");
    ArchiveMeta &meta = prepared->meta;
    meta.segment = o.segment;
    if (meta.segment == 0) {
        throw Error("the segment size must be positive");
    }
    for (size_t k = 0; k < inputs.size(); ++k) {
        if (places[k].frozen && places[k].carry) {
            SourceMeta sm = *places[k].carry;
            sm.position = k;
            meta.sources.push_back(std::move(sm));
            continue;
        }
        const CaptureStats &st = stats[k];
        const uint64_t covered = formatCovered(fmts[k], st.size);
        const std::optional<SourceMeta> &carry = places[k].carry;
        SourceMeta sm;
        sm.tail.resize(static_cast<size_t>(st.size - covered));
        if (!sm.tail.empty()) {
            PosFile f(inputs[k], PosFile::Mode::Read);
            f.readAt(sm.tail.data(), sm.tail.size(), covered);
        }
        sm.name = names[k];
        sm.size = st.size;
        sm.sha256 = st.sha;
        sm.words = formatSamples(fmts[k], st.size);
        sm.fmt = formatName(fmts[k]);
        sm.sampleSha256 = st.sampleSha;
        sm.shift = st.shift;
        sm.mean = st.mean;
        const Preset &preset = presets[k];
        sm.own = preset.own;
        sm.solo = preset.solo;
        sm.taps = preset.taps;
        sm.ntop = 0;
        sm.ttaps = preset.ttaps;
        sm.stride = preset.stride;
        sm.every = preset.every;
        sm.lamBits = preset.lamBits;
        for (size_t j = 0; j < k; ++j) {
            if (tables.count({k, j}) != 0) {
                sm.refs.push_back(j);
            }
        }
        sm.position = edit ? k : ord[k];
        sm.noise = noise[k];
        if (places[k].like) {
            // Coded as the track it is made from was.
            const SourceMeta &l = *places[k].like;
            sm.own = l.own;
            sm.solo = l.solo;
            sm.taps = l.taps;
            sm.ntop = l.ntop;
            sm.ttaps = l.ttaps;
            sm.stride = l.stride;
            sm.every = l.every;
            sm.lamBits = l.lamBits;
            sm.refs = l.refs;
            sm.weights = l.weights;
            sm.noise = l.noise;
        }
        sm.sidecars = carry ? carry->sidecars : findSidecars(origs[k]);
        if (carry && carry->rateHz > 0) {
            sm.rateHz = carry->rateHz;
        } else if (places[k].like && places[k].like->rateHz > 0) {
            sm.rateHz = places[k].like->rateHz;
        } else {
            const std::optional<double> hz = sidecarRateHz(sm.sidecars);
            sm.rateHz = hz ? *hz : o.rateMhz * 1e6;
        }
        if (carry) {
            sm.origin = carry->origin;
        } else if (origs[k] != inputs[k]) {
            sm.origin = QFileInfo(origs[k]).fileName().toStdString();
        }
        if (places[k].rateHz) {
            sm.rateHz = *places[k].rateHz;
        }
        if (places[k].notes) {
            sm.notes = *places[k].notes;
        }
        if (places[k].bits) {
            sm.bits = *places[k].bits;
        }
        // The note and the date of the file go with the track; a capture that is new gets the date of its file.
        if (carry) {
            sm.notes = carry->notes;
            sm.bits = carry->bits;
            sm.fileTime = carry->fileTime;
        } else if (places[k].like) {
            sm.notes = places[k].like->notes;
            sm.bits = places[k].like->bits;
            sm.fileTime = places[k].like->fileTime;
        } else {
            sm.fileTime = QFileInfo(origs[k]).lastModified().toUTC().toString(Qt::ISODate).toStdString();
        }
        meta.sources.push_back(std::move(sm));
    }
    for (const QString &p : inputs) {
        prepared->fileHolders.push_back(std::make_unique<PosFile>(p, PosFile::Mode::Read));
        prepared->files.push_back(prepared->fileHolders.back().get());
    }
    const std::vector<PosFile *> &files = prepared->files;

    phase("metadata and files");
    // Weigh the references of each capture by how well each predicts it.
    if (inputs.size() > 1) {
        const ReferenceWindows windows(files, meta, tables);
        std::vector<std::vector<size_t>> chosenRefs(inputs.size());
        std::vector<std::vector<float>> chosenWeights(inputs.size());
        for (size_t k = 0; k < inputs.size(); ++k) {
            if (!places[k].settled()) {
                chooseRefs(windows, files, meta, k, threads, chosenRefs[k], chosenWeights[k]);
            }
        }
        for (size_t k = 0; k < inputs.size(); ++k) {
            if (places[k].settled()) {
                continue;
            }
            for (const size_t j : meta.sources[k].refs) {
                if (std::find(chosenRefs[k].begin(), chosenRefs[k].end(), j) == chosenRefs[k].end()) {
                    tables.erase({k, j});
                }
            }
            meta.sources[k].refs = chosenRefs[k];
            meta.sources[k].weights = chosenWeights[k];
        }
        // With three references or more, try giving the best ones channels of
        // their own (and a shorter own history) against the plain mean, and keep
        // what costs less on a few windows.
        for (size_t k = 0; k < inputs.size(); ++k) {
            if (places[k].settled()) {
                continue;
            }
            SourceMeta &sm = meta.sources[k];
            const Preset &preset = presets[k];
            const size_t nr = sm.refs.size();
            sm.own = preset.plain;
            sm.ntop = 0;
            if (nr < kMinReferencesForChannels) {
                continue;
            }
            const double plain = trialCost(windows, files, meta, k, threads);
            // The channels take up the misalignment: try them with coarse tables.
            std::vector<std::pair<size_t, LagTable>> fine;
            for (const size_t j : sm.refs) {
                const auto it = tables.find({k, j});
                if (it != tables.end()) {
                    fine.emplace_back(j, it->second);
                }
            }
            for (const auto &f : fine) {
                std::optional<LagTable> t = buildLagTable(fields[k], fields[f.first], sm.words, kHysteresisCoarse);
                if (t) {
                    tables[{k, f.first}] = std::move(*t);
                }
            }
            sm.own = preset.own;
            sm.ntop = std::min(preset.ntop, nr);
            const double split = trialCost(windows, files, meta, k, threads);
            if (split >= plain) {
                sm.own = preset.plain;
                sm.ntop = 0;
                for (auto &f : fine) {
                    tables[{k, f.first}] = std::move(f.second);
                }
            }
        }
    }
    phase("references chosen");
    return prepared;
}

std::vector<PackedTrack> writePrepared(PreparedPack &prepared, const QString &output, const PackOptions &o,
                                       const std::vector<std::optional<CopiedTrack>> &copy)
{
    const unsigned threads = o.threads != 0 ? o.threads : defaultThreads();
    const ArchiveMeta &meta = prepared.meta;
    const std::vector<PosFile *> &files = prepared.files;
    QFile out(output);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw Error("creating " + output.toStdString() + ": " + out.errorString().toStdString());
    }
    writeArchiveHead(out, meta, prepared.tables);
    const ReferenceWindows windows(files, meta, prepared.tables);
    std::vector<PackedTrack> report;
    const size_t s = meta.segment;
    if (o.device == Device::Gpu) {
        // The tracks are independent when packing (their references are read from the
        // captures, not from what was coded), so the device works on one while the
        // processor codes another. They are written in order, each as soon as it and
        // the ones before it are done.
        const size_t n = meta.sources.size();
        std::vector<PackedTrack> done(n);
        std::mutex turnMutex;
        std::condition_variable turnChanged;
        size_t turn = 0;
        bool aborted = false;
        parallelFor(n, kGpuTracksAtOnce, [&](size_t k) {
            const SourceMeta &sm = meta.sources[k];
            try {
                std::vector<std::pair<uint64_t, std::vector<uint8_t>>> blocks;
                uint64_t packed = 0;
                double secs = 0;
                if (!copy[k]) {
                    const auto t0 = std::chrono::steady_clock::now();
                    packed = gpuPackTrack(k, meta, files, windows, o.verify, threads,
                                          [&blocks](uint64_t g, const std::vector<uint8_t> &bytes) { blocks.emplace_back(g, bytes); });
                    secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                }
                std::unique_lock<std::mutex> lock(turnMutex);
                turnChanged.wait(lock, [&] { return turn == k || aborted; });
                if (aborted) {
                    return;
                }
                if (copy[k]) {
                    packed = copyBlocks(out, *copy[k]->archive, copy[k]->track, k);
                    std::fprintf(stderr, "%s: %llu -> %llu bytes copied\n", sm.name.c_str(), static_cast<unsigned long long>(sm.size),
                                 static_cast<unsigned long long>(packed));
                } else {
                    for (const auto &b : blocks) {
                        writeAll(out, ebml::cluster(b.first, k + 1, b.second));
                    }
                    std::fprintf(stderr, "%s: %llu -> %llu bytes (%.3f bits/sample, %s%%), %zu reference(s), %.1f Msmp/s\n", sm.name.c_str(),
                                 static_cast<unsigned long long>(sm.size), static_cast<unsigned long long>(packed),
                                 static_cast<double>(packed) * 8.0 / static_cast<double>(std::max<uint64_t>(sm.words, 1)),
                                 perCent(static_cast<double>(packed), static_cast<double>(sm.size)).c_str(), sm.refs.size(),
                                 static_cast<double>(sm.words) / std::max(secs, 1e-9) / 1e6);
                }
                done[k] = {sm.name, sm.size, packed, sm.refs.size()};
                ++turn;
                turnChanged.notify_all();
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(turnMutex);
                    aborted = true;
                }
                turnChanged.notify_all();
                throw;
            }
        });
        if (!out.flush()) {
            throw Error("cannot write " + output.toStdString() + ": " + out.errorString().toStdString());
        }
        return done;
    }
    for (size_t k = 0; k < meta.sources.size(); ++k) {
        const SourceMeta &sm = meta.sources[k];
        if (copy[k]) {
            const uint64_t packed = copyBlocks(out, *copy[k]->archive, copy[k]->track, k);
            std::fprintf(stderr, "%s: %llu -> %llu bytes copied\n", sm.name.c_str(), static_cast<unsigned long long>(sm.size),
                         static_cast<unsigned long long>(packed));
            report.push_back({sm.name, sm.size, packed, sm.refs.size()});
            continue;
        }
        const Preset pre = sm.preset();
        const bool useRef = !sm.refs.empty();
        const size_t nseg = static_cast<size_t>((sm.words + s - 1) / s);
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t packed = 0;
        const size_t chunk = static_cast<size_t>(threads) * 4;
        if (o.device == Device::Gpu) {
            packed = gpuPackTrack(k, meta, files, windows, o.verify, threads, [&](uint64_t g, const std::vector<uint8_t> &bytes) { writeAll(out, ebml::cluster(g, k + 1, bytes)); });
        }
        for (size_t first = 0; first < (o.device == Device::Gpu ? 0 : nseg); first += chunk) {
            const size_t count = std::min(chunk, nseg - first);
            std::vector<std::vector<uint8_t>> blocks(count);
            parallelFor(count, threads, [&](size_t i) {
                const size_t g = first + i;
                const size_t s0 = g * s;
                const size_t n = std::min<size_t>(s, static_cast<size_t>(sm.words) - s0);
                const std::vector<int32_t> y = trackSamples(*files[k], sm, static_cast<int64_t>(s0), n);
                std::vector<float> window;
                if (useRef) {
                    window = windows.window(k, static_cast<int64_t>(s0), n);
                }
                const size_t stride = n + 2 * sm.taps;
                blocks[i] = encodeResiduals(predictionErrors(y, pre, useRef, window.data(), stride));
                if (o.verify) {
                    std::vector<int32_t> residuals;
                    if (!decodeResiduals(blocks[i].data(), blocks[i].size(), n, residuals)
                        || reconstruct(residuals, pre, useRef, window.data(), stride) != y) {
                        throw Error("self-check failed: segment " + std::to_string(g) + " of " + sm.name + " does not decode to itself");
                    }
                }
            });
            for (size_t i = 0; i < count; ++i) {
                packed += blocks[i].size();
                writeAll(out, ebml::cluster(first + i, k + 1, blocks[i]));
            }
            std::fprintf(stderr, "coding %s: %d%%\r", sm.name.c_str(), static_cast<int>(100.0 * static_cast<double>(std::min(first + count, nseg)) / static_cast<double>(std::max<size_t>(nseg, 1))));
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "%s: %llu -> %llu bytes (%.3f bits/sample, %s%%), %zu reference(s), %.1f Msmp/s\n", sm.name.c_str(),
                     static_cast<unsigned long long>(sm.size), static_cast<unsigned long long>(packed),
                     static_cast<double>(packed) * 8.0 / static_cast<double>(std::max<uint64_t>(sm.words, 1)),
                     perCent(static_cast<double>(packed), static_cast<double>(sm.size)).c_str(), sm.refs.size(),
                     static_cast<double>(sm.words) / std::max(secs, 1e-9) / 1e6);
        report.push_back({sm.name, sm.size, packed, sm.refs.size()});
    }
    if (!out.flush()) {
        throw Error("cannot write " + output.toStdString() + ": " + out.errorString().toStdString());
    }
    return report;
}

std::vector<PackedTrack> packCaptures(const std::vector<QString> &inputs, const QString &output, const PackOptions &options)
{
    const std::vector<PackSpec> specs(inputs.size());
    std::unique_ptr<PreparedPack> prepared = prepareCaptures(inputs, specs, {}, output, options);
    return writePrepared(*prepared, output, options, std::vector<std::optional<CopiedTrack>>(inputs.size()));
}

std::vector<PackedTrack> packCapturesLike(const std::vector<QString> &inputs, const QString &archivePath, const std::vector<size_t> &tracks,
                                          const QString &output, const PackOptions &options, std::optional<double> rateHz)
{
    if (inputs.empty() || inputs.size() != tracks.size()) {
        throw Error("every capture needs the track of the archive it is made from");
    }
    Archive like(archivePath);
    const std::vector<SourceMeta> &sources = like.meta().sources;
    std::map<size_t, size_t> placeOf;
    for (size_t k = 0; k < tracks.size(); ++k) {
        if (tracks[k] >= sources.size()) {
            throw Error(archivePath.toStdString() + " has " + std::to_string(sources.size()) + " tracks: no track " + std::to_string(tracks[k]));
        }
        if (k > 0 && tracks[k] <= tracks[k - 1]) {
            throw Error("the tracks must be given in the order of the archive");
        }
        placeOf[tracks[k]] = k;
        const SourceMeta &sm = sources[tracks[k]];
        if (formatSamples(sm.format(), static_cast<uint64_t>(QFileInfo(inputs[k]).size())) != sm.words) {
            throw Error(inputs[k].toStdString() + " does not have the samples of track " + std::to_string(tracks[k]) + " of the archive");
        }
    }

    // What each track keeps of the archive's coding: the references that are still
    // there, with their weights as they were, and the lag tables against them.
    std::vector<PackSpec> specs(inputs.size());
    LagTables known;
    for (size_t k = 0; k < tracks.size(); ++k) {
        SourceMeta m = sources[tracks[k]];
        std::vector<size_t> refs;
        std::vector<float> weights;
        bool dropped = false;
        for (size_t i = 0; i < m.refs.size(); ++i) {
            const auto place = placeOf.find(m.refs[i]);
            if (place == placeOf.end()) {
                dropped = true;
                continue;
            }
            const auto table = like.tables().find({tracks[k], m.refs[i]});
            if (table == like.tables().end()) {
                throw Error(m.name + ": lag table against track " + std::to_string(m.refs[i]) + " is missing");
            }
            known[{k, place->second}] = table->second;
            refs.push_back(place->second);
            if (!m.weights.empty()) {
                weights.push_back(m.weights[i]);
            }
        }
        if (dropped && !weights.empty()) {
            // They add up to 1 again.
            float sum = 0.0F;
            for (const float w : weights) {
                sum += w;
            }
            for (float &w : weights) {
                w /= sum;
            }
        }
        // (a track left with no reference is coded from its own past, as it was
        // when it had none: `solo` is its history, and `ntop` has nothing to take)
        m.refs = refs;
        m.weights = weights;
        m.ntop = std::min(m.ntop, refs.size());
        if (rateHz) {
            m.rateHz = *rateHz;
        }
        specs[k].like = std::move(m);
    }

    PackOptions o = options;
    o.order = OrderMode::Given;
    o.format = "auto";
    // Cut into segments as the archive was.
    o.segment = like.meta().segment;
    std::unique_ptr<PreparedPack> prepared = prepareCaptures(inputs, specs, known, output, o);
    return writePrepared(*prepared, output, o, std::vector<std::optional<CopiedTrack>>(inputs.size()));
}

} // namespace rfx
