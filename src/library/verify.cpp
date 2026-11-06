/******************************************************************************
 * verify.cpp
 * rfx library - checking an archive against the checksums it keeps
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "verify.h"

#include "error.h"
#include "sha256.h"
#include "trackreader.h"

#include <QtGlobal>

#include <algorithm>
#include <mutex>
#include <thread>

namespace rfx {

namespace {

// Samples read at a time (a long stretch is decoded on all the processors by the reader).
constexpr size_t kChunk = size_t(1) << 22;

// Feed samples to the hash as little-endian i32, as the hash of the samples is defined.
void hashBlock(Sha256 &hash, const int32_t *samples, size_t n, std::vector<uint8_t> &scratch)
{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    (void) scratch;
    hash.update(samples, n * 4);
#else
    scratch.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = static_cast<uint32_t>(samples[i]);
        scratch[4 * i] = static_cast<uint8_t>(v);
        scratch[4 * i + 1] = static_cast<uint8_t>(v >> 8);
        scratch[4 * i + 2] = static_cast<uint8_t>(v >> 16);
        scratch[4 * i + 3] = static_cast<uint8_t>(v >> 24);
    }
    hash.update(scratch.data(), scratch.size());
#endif
}

std::string hexOf(const std::vector<uint8_t> &data)
{
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.finishHex();
}

} // namespace

std::vector<TrackCheck> verifyArchive(const QString &path, std::optional<size_t> only, const std::function<void(double)> &progress, const std::atomic<bool> *stop)
{
    TrackReader reader(path);
    if (!reader.isArchive()) {
        throw Error(path.toStdString() + ": not an .rfx archive");
    }
    const size_t count = reader.meta().sources.size();
    if (only && *only >= count) {
        throw Error(path.toStdString() + ": the archive has " + std::to_string(count) + " tracks");
    }
    std::vector<size_t> todo;
    for (size_t k = 0; k < count; ++k) {
        if (!only || *only == k) {
            todo.push_back(k);
        }
    }
    std::vector<TrackCheck> checks(count);
    std::vector<double> part(count, 0.0);
    std::mutex mutex;
    const auto report = [&] {
        if (!progress) {
            return;
        }
        double sum = 0.0;
        for (const size_t k : todo) {
            sum += part[k];
        }
        progress(sum / static_cast<double>(todo.size()));
    };
    std::vector<std::thread> workers;
    for (const size_t k : todo) {
        workers.emplace_back([&, k] {
            const SourceMeta &sm = reader.meta().sources[k];
            TrackCheck check;
            check.track = k;
            check.name = sm.name;
            try {
                // The samples, against the hash of the samples.
                Sha256 hash;
                std::vector<int32_t> buf(kChunk);
                std::vector<uint8_t> scratch;
                bool stopped = false;
                for (uint64_t at = 0; at < sm.words; at += buf.size()) {
                    if (stop && stop->load()) {
                        stopped = true;
                        break;
                    }
                    const size_t n = static_cast<size_t>(std::min<uint64_t>(sm.words - at, buf.size()));
                    reader.read(k, static_cast<int64_t>(at), n, buf.data());
                    hashBlock(hash, buf.data(), n, scratch);
                    std::lock_guard<std::mutex> lock(mutex);
                    part[k] = static_cast<double>(at + n) / static_cast<double>(std::max<uint64_t>(sm.words, 1));
                    report();
                }
                if (stopped) {
                    check.detail = "not checked: stopped";
                } else {
                    const std::string got = hash.finishHex();
                    check.checked = !sm.sampleSha256.empty();
                    check.ok = true;
                    if (!check.checked) {
                        check.detail = "no checksum of the samples is kept in the file";
                    } else if (got != sm.sampleSha256) {
                        check.ok = false;
                        check.detail = "the samples differ from the ones that were packed (SHA-256 " + got + " against " + sm.sampleSha256 + ")";
                    }
                }
                // The copies of the files that sat beside the capture.
                for (const Sidecar &sc : sm.sidecars) {
                    if (check.ok && !stopped && hexOf(sc.data) != sc.sha256) {
                        check.ok = false;
                        check.detail = "the stored copy of " + sc.name + " is damaged";
                    }
                }
            } catch (const std::exception &e) {
                check.ok = false;
                check.checked = true;
                check.detail = std::string("cannot be decoded: ") + e.what();
            }
            std::lock_guard<std::mutex> lock(mutex);
            part[k] = 1.0;
            report();
            checks[k] = std::move(check);
        });
    }
    for (std::thread &w : workers) {
        w.join();
    }
    std::vector<TrackCheck> out;
    for (const size_t k : todo) {
        out.push_back(std::move(checks[k]));
    }
    return out;
}

} // namespace rfx
