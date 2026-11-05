/******************************************************************************
 * edit.cpp
 * rfx library - editing archives
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "edit.h"

#include "archive.h"
#include "capture.h"
#include "error.h"
#include "flacmode.h"
#include "packer_internal.h"

#include <QDir>
#include <QFile>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>

namespace rfx {

namespace {

// Removes a directory when it goes out of scope.
struct RemoveDirectory
{
    QString path;
    ~RemoveDirectory()
    {
        if (!path.isEmpty()) {
            QDir(path).removeRecursively();
        }
    }
};

// The tracks of a file of any variant.
size_t trackCount(const QString &path)
{
    return isFlacArchive(path) ? readFlacMeta(path).size() : Archive(path).meta().sources.size();
}

void checkTracks(const QString &archive, size_t tracks, const std::vector<size_t> &which)
{
    for (const size_t i : which) {
        if (i >= tracks) {
            throw Error(archive.toStdString() + " has " + std::to_string(tracks) + " tracks: no track " + std::to_string(i));
        }
    }
}

} // namespace

void assembleArchives(const std::vector<QString> &archives, const std::vector<Pick> &picks, const QString &output,
                      const PackOptions &requested, std::optional<size_t> recodeFrom)
{
    if (picks.empty()) {
        throw Error("nothing to write: no track selected");
    }
    // Files of the FLAC or raw variant have no references to keep or to lose: they are written again in the variant of the first, with
    // what is asked changed.
    if (std::any_of(archives.begin(), archives.end(), [](const QString &a) { return isFlacArchive(a); })) {
        if (!std::all_of(archives.begin(), archives.end(), [](const QString &a) { return isFlacArchive(a); })) {
            throw Error("files of the predictor and files of the FLAC or raw variant cannot be written together");
        }
        std::string stored;
        readFlacMeta(archives.front(), &stored);
        assembleStored(archives, picks, output, requested, stored == "raw" ? StoredCodec::Raw : StoredCodec::Flac);
        return;
    }
    std::vector<std::unique_ptr<Archive>> opened;
    for (const QString &p : archives) {
        opened.push_back(std::make_unique<Archive>(p));
    }
    if (!opened.empty()) {
        for (const auto &b : opened) {
            if (b->meta().segment != opened.front()->meta().segment) {
                throw Error("the archives do not use the same segment length");
            }
        }
    }
    PackOptions o = requested;
    o.order = OrderMode::Given;
    o.format = "auto";
    const size_t segment = opened.empty() ? o.segment : opened.front()->meta().segment;
    o.segment = segment;

    // Where each old track goes.
    std::map<std::pair<size_t, size_t>, size_t> place;
    for (size_t k = 0; k < picks.size(); ++k) {
        const Pick &p = picks[k];
        if (!p.fromArchive) {
            continue;
        }
        if (p.arch >= opened.size() || p.src >= opened[p.arch]->meta().sources.size()) {
            throw Error("no track " + std::to_string(p.src) + " in archive " + std::to_string(p.arch));
        }
        if (!place.emplace(std::make_pair(p.arch, p.src), k).second) {
            throw Error("a track is selected twice");
        }
    }
    // The spec of every pick, with the coding kept when it stays valid.
    std::vector<PackSpec> specs;
    LagTables known;
    std::vector<std::optional<CopiedTrack>> copy;
    for (size_t k = 0; k < picks.size(); ++k) {
        const Pick &p = picks[k];
        if (!p.fromArchive) {
            PackSpec fresh;
            fresh.level = p.edit.level;
            fresh.rateHz = p.edit.rateHz;
            fresh.notes = p.edit.notes;
            fresh.bits = p.edit.bits;
            specs.push_back(fresh);
            copy.emplace_back();
            continue;
        }
        Archive &a = *opened[p.arch];
        // The track as it will be written: what was asked changes its metadata, nothing else.
        SourceMeta sm = a.meta().sources[p.src];
        if (p.edit.name) {
            sm.name = *p.edit.name;
        }
        if (p.edit.rateHz) {
            sm.rateHz = *p.edit.rateHz;
        }
        if (p.edit.notes) {
            sm.notes = *p.edit.notes;
        }
        if (p.edit.bits) {
            sm.bits = *p.edit.bits;
        }
        if (p.edit.sidecars) {
            sm.sidecars = *p.edit.sidecars;
        }
        std::vector<size_t> moved;
        bool allMoved = true;
        for (const size_t j : sm.refs) {
            const auto it = place.find({p.arch, j});
            if (it == place.end() || it->second >= k) {
                allMoved = false;
                break;
            }
            moved.push_back(it->second);
        }
        const bool forced = (recodeFrom && p.arch >= *recodeFrom) || p.edit.recode;
        if (allMoved && !forced) {
            SourceMeta m = sm;
            m.refs = moved;
            for (size_t i = 0; i < sm.refs.size(); ++i) {
                const auto t = a.tables().find({p.src, sm.refs[i]});
                if (t == a.tables().end()) {
                    throw Error(sm.name + ": lag table against track " + std::to_string(sm.refs[i]) + " is missing");
                }
                known[{k, moved[i]}] = t->second;
            }
            specs.push_back({m, true});
            copy.push_back(CopiedTrack{&a, p.src});
        } else {
            PackSpec again{sm, false};
            again.level = p.edit.level;
            specs.push_back(again);
            copy.emplace_back();
        }
    }
    const size_t kept = static_cast<size_t>(std::count_if(copy.begin(), copy.end(), [](const auto &c) { return c.has_value(); }));
    std::fprintf(stderr, "%zu track(s) to write: %zu copied as they are, %zu to code\n", picks.size(), kept, picks.size() - kept);

    const QString tmp = withExtension(output, QStringLiteral("rfx-tmp"));
    try {
        if (kept == picks.size()) {
            // Nothing to code: the blocks and the tables, renumbered.
            ArchiveMeta meta;
            meta.segment = segment;
            std::set<std::string> names;
            for (size_t k = 0; k < specs.size(); ++k) {
                SourceMeta m = *specs[k].carry;
                m.position = k;
                if (!names.insert(m.name).second) {
                    throw Error("two tracks are named " + m.name + ": the archive keeps file names only");
                }
                meta.sources.push_back(std::move(m));
            }
            QFile out(tmp);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                throw Error("creating " + tmp.toStdString() + ": " + out.errorString().toStdString());
            }
            writeArchiveHead(out, meta, known);
            for (size_t k = 0; k < copy.size(); ++k) {
                copyBlocks(out, *copy[k]->archive, copy[k]->track, k);
            }
            if (!out.flush()) {
                throw Error("cannot write " + tmp.toStdString());
            }
        } else {
            // Coding needed: the archives' tracks become raw files to read the
            // references from.
            const QString workPath = withExtension(output, QStringLiteral("edit"));
            QDir().mkpath(workPath);
            RemoveDirectory cleanup{workPath};
            std::map<size_t, QString> dirs;
            for (const Pick &p : picks) {
                if (p.fromArchive && dirs.count(p.arch) == 0) {
                    const QString d = QDir(workPath).filePath(QStringLiteral("a%1").arg(static_cast<qulonglong>(p.arch)));
                    std::fprintf(stderr, "decoding %s to read the references\n", archives[p.arch].toStdString().c_str());
                    UnpackOptions decodeOptions;
                    decodeOptions.device = o.device;
                    decodeOptions.threads = o.threads;
                    unpackArchive(archives[p.arch], d, decodeOptions);
                    dirs[p.arch] = d;
                }
            }
            std::vector<QString> given;
            for (const Pick &p : picks) {
                given.push_back(p.fromArchive ? QDir(dirs[p.arch]).filePath(QString::fromStdString(opened[p.arch]->meta().sources[p.src].name)) : p.capture);
            }
            {
                std::unique_ptr<PreparedPack> prepared = prepareCaptures(given, specs, known, tmp, o);
                writePrepared(*prepared, tmp, o, copy);
            }
        }
    } catch (...) {
        opened.clear();
        QFile::remove(tmp);
        throw;
    }
    opened.clear();
    if (QFile::exists(output) && !QFile::remove(output)) {
        throw Error("cannot replace " + output.toStdString());
    }
    if (!QFile::rename(tmp, output)) {
        throw Error("writing " + output.toStdString());
    }
}

std::vector<Pick> pickSome(const QString &archive, const std::vector<size_t> &which)
{
    const size_t n = trackCount(archive);
    checkTracks(archive, n, which);
    const std::set<size_t> set(which.begin(), which.end());
    std::vector<Pick> picks;
    for (size_t i = 0; i < n; ++i) {
        if (set.count(i) != 0) {
            picks.push_back(Pick::old(0, i));
        }
    }
    return picks;
}

std::vector<Pick> pickWithout(const QString &archive, const std::vector<size_t> &which)
{
    const size_t n = trackCount(archive);
    checkTracks(archive, n, which);
    const std::set<size_t> set(which.begin(), which.end());
    std::vector<Pick> picks;
    for (size_t i = 0; i < n; ++i) {
        if (set.count(i) == 0) {
            picks.push_back(Pick::old(0, i));
        }
    }
    return picks;
}

std::vector<Pick> pickAll(const std::vector<QString> &archives)
{
    std::vector<Pick> picks;
    for (size_t arch = 0; arch < archives.size(); ++arch) {
        const size_t n = trackCount(archives[arch]);
        for (size_t src = 0; src < n; ++src) {
            picks.push_back(Pick::old(arch, src));
        }
    }
    return picks;
}

} // namespace rfx
