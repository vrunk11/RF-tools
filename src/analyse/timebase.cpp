/******************************************************************************
 * timebase.cpp
 * rf-analyse - the time base of a capture corrected from the lines pass 0 located
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "timebase.h"

#include "rfsync.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>

namespace {

std::string keyOf(const rfx::Field &f)
{
    return *f.code + (f.first ? "/1" : "/0");
}

} // namespace

double TimeBase::sourceAt(double m) const
{
    if (x.empty()) {
        return m;
    }
    if (m <= tau.front()) {
        return x.front() + (m - tau.front()) * slopeFirst;
    }
    if (m >= tau.back()) {
        return x.back() + (m - tau.back()) * slopeLast;
    }
    const size_t i = static_cast<size_t>(std::upper_bound(tau.begin(), tau.end(), m) - tau.begin());
    return x[i - 1] + (m - tau[i - 1]) / (tau[i] - tau[i - 1]) * (x[i] - x[i - 1]);
}

std::shared_ptr<const TimeBase> TimeBase::make(const std::vector<rfx::Field> &own, bool pal, double rate, const std::vector<rfx::Field> *anchor)
{
    if (own.empty() || own.front().lines.empty()) {
        return nullptr;
    }
    const double nominal = rfx::nominalLineSamples(rate, pal ? rfx::VideoSystem::Pal : rfx::VideoSystem::Ntsc);
    const double fieldLines = pal ? 312.5 : 262.5;

    // Which field of the common axis each field of the capture is.
    const std::vector<rfx::Field> &reference = anchor && !anchor->empty() && !anchor->front().lines.empty() ? *anchor : own;
    std::unordered_map<std::string, std::vector<int64_t>> where;
    for (size_t j = 0; j < reference.size(); ++j) {
        if (reference[j].code) {
            where[keyOf(reference[j])].push_back(static_cast<int64_t>(j));
        }
    }
    // (the parity of the field number follows the first fields: if the axis starts on a second field, its fields are numbered from one)
    const int64_t shift = reference.front().first ? 0 : 1;
    std::vector<int64_t> number(own.size(), 0);
    std::vector<bool> known(own.size(), false);
    size_t matched = 0;
    if (&reference == &own) {
        for (size_t i = 0; i < own.size(); ++i) {
            number[i] = static_cast<int64_t>(i);
            known[i] = true;
        }
    } else {
        // A frame code may be read wrong, or be the same in two places of the axis: each field has the offsets (its number on the axis
        // minus its place in the capture) its code allows, and takes the one its neighbours agree on. A match no neighbour confirms is
        // left out, so that the field follows the ones next to it.
        std::vector<std::vector<int64_t>> allowed(own.size());
        for (size_t i = 0; i < own.size(); ++i) {
            if (own[i].code) {
                const auto it = where.find(keyOf(own[i]));
                if (it != where.end()) {
                    for (const int64_t j : it->second) {
                        allowed[i].push_back(j - static_cast<int64_t>(i));
                    }
                }
            }
        }
        constexpr int kReach = 8;
        for (size_t i = 0; i < own.size(); ++i) {
            std::map<int64_t, int> votes;
            const size_t from = i > kReach ? i - kReach : 0;
            const size_t to = std::min(own.size(), i + kReach + 1);
            for (size_t t = from; t < to; ++t) {
                for (const int64_t o : allowed[t]) {
                    ++votes[o];
                }
            }
            if (votes.empty()) {
                continue;
            }
            int64_t chosen = 0;
            int best = 0;
            for (const int64_t o : allowed[i]) {
                if (votes[o] >= 2 && votes[o] > best) {
                    best = votes[o];
                    chosen = o;
                }
            }
            if (best == 0) {
                for (const auto &v : votes) {
                    if (v.second > best) {
                        best = v.second;
                        chosen = v.first;
                    }
                }
            }
            number[i] = static_cast<int64_t>(i) + chosen;
            known[i] = true;
            ++matched;
        }        if (matched == 0) {
            // Nothing in common: on its own axis.
            for (size_t i = 0; i < own.size(); ++i) {
                number[i] = static_cast<int64_t>(i);
                known[i] = true;
            }
        } else {
            // The fields follow one another: a field with no match is the one after the one before, or before the one after.
            for (size_t i = 1; i < own.size(); ++i) {
                if (!known[i] && known[i - 1]) {
                    number[i] = number[i - 1] + 1;
                    known[i] = true;
                }
            }
            for (size_t i = own.size(); i-- > 1;) {
                if (!known[i - 1] && known[i]) {
                    number[i - 1] = number[i] - 1;
                    known[i - 1] = true;
                }
            }
        }
    }

    // Where the axis starts: the first line of its first field, in the capture it comes from.
    const double origin = reference.front().lines.front();

    // Every line at its place.
    std::map<int64_t, double> lines;
    for (size_t i = 0; i < own.size(); ++i) {
        if (!known[i]) {
            continue;
        }
        const double start = static_cast<double>(number[i] + shift) * fieldLines;
        // Only the lines of the field itself: the record carries the first lines of the next one too (the same pulses, found again), which are
        // taken from that one.
        double limit = 1e300;
        if (i + 1 < own.size() && known[i + 1] && number[i + 1] == number[i] + 1 && !own[i + 1].lines.empty()) {
            limit = own[i + 1].lines.front() - 0.25 * nominal;
        }
        for (size_t n = 0; n < own[i].lines.size() && own[i].lines[n] < limit; ++n) {
            lines.insert_or_assign(static_cast<int64_t>(std::llround((start + static_cast<double>(n)) * 2.0)), own[i].lines[n]);
        }
    }
    auto out = std::make_shared<TimeBase>();
    for (const auto &entry : lines) {
        const double position = entry.second;
        // A line that goes backwards is a misreading: left out.
        if (!out->x.empty() && position <= out->x.back()) {
            continue;
        }
        out->tau.push_back(origin + (static_cast<double>(entry.first) / 2.0 - static_cast<double>(shift) * fieldLines) * nominal);
        out->x.push_back(position);
    }
    if (out->x.size() < 2) {
        return nullptr;
    }
    const size_t n = out->x.size();
    out->slopeFirst = (out->x[1] - out->x[0]) / (out->tau[1] - out->tau[0]);
    out->slopeLast = (out->x[n - 1] - out->x[n - 2]) / (out->tau[n - 1] - out->tau[n - 2]);
    return out;
}
