/******************************************************************************
 * alignment.cpp
 * rfx library - pass 0 dumps and the lag tables built from them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "alignment.h"

#include "error.h"

#include <QDir>
#include <QLocale>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rfx {

namespace {

// Positions of the lag table lie on a grid of this many samples.
constexpr uint64_t kGrid = 64;
// Lines over which the line-to-line lag is averaged.
constexpr double kSmoothLines = 16.0;
// More lines than this make a field a real one (to measure the length of a line).
constexpr size_t kFullFieldLines = 10;

// The key of a field: its frame code and whether it is the first of the frame.
std::string keyOf(const std::string &code, bool first)
{
    return code + (first ? '1' : '0');
}

} // namespace

std::vector<Field> parsePass0(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        throw Error("reading " + path.toStdString() + ": " + f.errorString().toStdString());
    }
    const QByteArray text = f.readAll();
    const std::string bad = path.toStdString() + ": unexpected line in the pass 0 dump";
    std::vector<Field> out;
    const char *p = text.constData();
    const char *end = p + text.size();
    while (p < end) {
        const char *eol = static_cast<const char *>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
        const char *lineEnd = eol ? eol : end;
        const char *stop = lineEnd;
        if (stop > p && stop[-1] == '\r') {
            --stop;
        }
        Field field;
        const char *q = p;
        if (stop - q >= 5 && std::memcmp(q, "true ", 5) == 0) {
            field.first = true;
            q += 5;
        } else if (stop - q >= 6 && std::memcmp(q, "false ", 6) == 0) {
            field.first = false;
            q += 6;
        } else {
            throw Error(bad);
        }
        if (stop - q >= 4 && std::memcmp(q, "None", 4) == 0) {
            q += 4;
        } else {
            const std::string rest(q, static_cast<size_t>(stop - q));
            const size_t close = rest.find("])");
            if (close == std::string::npos) {
                throw Error("bad frame code in the pass 0 dump");
            }
            field.code = rest.substr(0, close + 2);
            q += close + 2;
        }
        while (q < stop) {
            while (q < stop && std::isspace(static_cast<unsigned char>(*q))) {
                ++q;
            }
            if (q >= stop) {
                break;
            }
            uint64_t bits = 0;
            int digits = 0;
            while (q < stop && std::isxdigit(static_cast<unsigned char>(*q))) {
                const char c = *q;
                bits = (bits << 4) | static_cast<uint64_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
                ++q;
                ++digits;
            }
            if (digits == 0 || digits > 16 || (q < stop && !std::isspace(static_cast<unsigned char>(*q)))) {
                throw Error("bad line position");
            }
            double v;
            std::memcpy(&v, &bits, sizeof(v));
            field.lines.push_back(v);
        }
        out.push_back(std::move(field));
        p = lineEnd + 1;
    }
    return out;
}

void runPass0(const QString &ldDecode, const std::vector<QString> &inputs, double rateMhz, const std::string &format,
              const QString &dir)
{
    QDir().mkpath(dir);
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LD_DUMP_PASS0"), dir);
    env.insert(QStringLiteral("LD_PASS0_ONLY"), QStringLiteral("1"));
    proc.setProcessEnvironment(env);
    QStringList args = {"--format", QString::fromStdString(format), "--inputfreq", QString::number(rateMhz, 'g', QLocale::FloatingPointShortest)};
    for (const QString &i : inputs) {
        args << "-i" << i;
    }
    args << "-l" << "1" << "-o" << QDir(dir).filePath(QStringLiteral("pass0_probe"));
    proc.start(ldDecode, args);
    if (!proc.waitForStarted()) {
        throw Error("starting " + ldDecode.toStdString());
    }
    proc.waitForFinished(-1);
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        throw Error("ld-decode failed: " + proc.readAllStandardError().toStdString());
    }
}

std::optional<LagTable> buildLagTable(const std::vector<Field> &target, const std::vector<Field> &reference,
                                      uint64_t targetWords, double hysteresis)
{
    std::unordered_map<std::string, size_t> firstRef;
    for (size_t i = 0; i < reference.size(); ++i) {
        if (reference[i].code) {
            firstRef.emplace(keyOf(*reference[i].code, reference[i].first), i);
        }
    }
    std::unordered_set<std::string> seen;
    std::vector<std::pair<double, double>> pts;
    for (const Field &f : target) {
        if (!f.code) {
            continue;
        }
        const std::string key = keyOf(*f.code, f.first);
        if (!seen.insert(key).second) {
            continue;
        }
        const auto it = firstRef.find(key);
        if (it != firstRef.end()) {
            const std::vector<double> &rl = reference[it->second].lines;
            const size_t n = std::min(f.lines.size(), rl.size());
            for (size_t i = 0; i < n; ++i) {
                pts.emplace_back(f.lines[i], rl[i]);
            }
        }
    }
    if (pts.size() < 2) {
        return std::nullopt;
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    pts.erase(std::unique(pts.begin(), pts.end(), [](const auto &a, const auto &b) { return a.first == b.first; }), pts.end());
    double lineLength = 0;
    {
        const auto full = std::find_if(target.begin(), target.end(), [](const Field &f) { return f.lines.size() > kFullFieldLines; });
        if (full == target.end()) {
            return std::nullopt;
        }
        const std::vector<double> &l = full->lines;
        lineLength = (l.back() - l.front()) / static_cast<double>(l.size() - 1);
    }
    if (pts.size() < 2) {
        return std::nullopt;
    }
    const size_t ng = static_cast<size_t>(targetWords / kGrid + 2);
    auto at = [&pts](double p) -> double {
        if (p <= pts[0].first) {
            return pts[0].second - pts[0].first;
        }
        const std::pair<double, double> last = pts.back();
        if (p >= last.first) {
            return last.second - last.first;
        }
        size_t i = static_cast<size_t>(std::partition_point(pts.begin(), pts.end(), [p](const auto &x) { return x.first < p; }) - pts.begin());
        i = std::min(std::max<size_t>(i, 1), pts.size() - 1);
        const double a0 = pts[i - 1].first;
        const double b0 = pts[i - 1].second;
        const double a1 = pts[i].first;
        const double b1 = pts[i].second;
        return b0 + (b1 - b0) * (p - a0) / (a1 - a0) - p;
    };
    std::vector<double> raw(ng);
    for (size_t g = 0; g < ng; ++g) {
        raw[g] = at(static_cast<double>(static_cast<uint64_t>(g) * kGrid));
    }
    std::vector<double> prefix(ng + 1, 0.0);
    for (size_t g = 0; g < ng; ++g) {
        prefix[g + 1] = prefix[g] + raw[g];
    }
    const double halfWidth = kSmoothLines * lineLength / 2.0 / static_cast<double>(kGrid);
    const size_t h = halfWidth > 0 ? static_cast<size_t>(halfWidth) : 0;
    LagTable t;
    int64_t d = 0;
    for (size_t g = 0; g < ng; ++g) {
        const size_t a = g >= h ? g - h : 0;
        const size_t b = std::min(g + h + 1, ng);
        const double l = (prefix[b] - prefix[a]) / static_cast<double>(b - a);
        if (g == 0 || std::fabs(l - static_cast<double>(d)) > hysteresis) {
            const int64_t nd = static_cast<int64_t>(std::llround(l));
            if (g == 0 || nd != d) {
                t.pos.push_back(static_cast<uint64_t>(g) * kGrid);
                t.lag.push_back(nd);
                d = nd;
            }
        }
    }
    return t;
}

} // namespace rfx
