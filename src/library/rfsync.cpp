/******************************************************************************
 * rfsync.cpp
 * rfx library - pass 0 straight from the RF
 *
 * A port of `crates/ld-decode-cli/src/rfsync.rs` of ld-decode-rust (and of what it uses of `stack.rs`): the same filters, in the
 * same order of operations, so that the positions come out the same. Arithmetic is IEEE single or double precision with no
 * fused operations (the project builds with -ffp-contract=off).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "rfsync.h"

#include "error.h"
#include "parallel.h"

#include "archive.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rfx {

namespace {

constexpr double kPi = 3.14159265358979323846;

// What differs between the systems.
struct System
{
    bool pal = false;
    // Frequency the lines are locked onto: NTSC fsc (the burst), PAL pilot.
    double lockHz = 0;
    // Where it is measured, in us after the sync edge.
    std::pair<double, double> lockUs;
    // Carrier frequency step per IRE, blanking and sync tip (Hz).
    double hzIre = 0;
    double blankHz = 0;
    double syncHz = 0;
    double lineUs = 0;
    // Coarse and fine pass bands (Hz).
    std::pair<double, double> coarseBand;
    std::pair<double, double> fineBand;
    // Back porch window for the blanking level, us after the sync start.
    std::pair<double, double> porchUs;
    // VBI candidates: lines after the last broad pulse; and the lines after a field's first whose codes it takes.
    std::pair<double, double> vbiAfterV;
    std::pair<size_t, size_t> vbiLines;
    // Lines from a first field's start to the next field's, and from a second field's.
    std::pair<size_t, size_t> fieldLines;
};

System ntsc()
{
    const double fsc = 315.0e6 / 88.0;
    const double hzIre = 1.7e6 / 140.0;
    System s;
    s.pal = false;
    s.lockHz = fsc;
    // Burst cycles 21..28 after the edge.
    s.lockUs = {21.0e6 / fsc, 28.0e6 / fsc};
    s.hzIre = hzIre;
    s.blankHz = 8.1e6;
    s.syncHz = 8.1e6 - 40.0 * hzIre;
    s.lineUs = 227.5e6 / fsc;
    s.coarseBand = {4.5e6, 12.5e6};
    // Keeps the burst sidebands around the blanking carrier, 4.5 to 11.7 MHz, and rejects the 2.8 MHz audio carrier.
    s.fineBand = {3.8e6, 13.0e6};
    s.porchUs = {8.0, 9.0};
    s.vbiAfterV = {6.0, 14.0};
    s.vbiLines = {12, 20};
    s.fieldLines = {263, 262};
    return s;
}

System pal()
{
    const double hzIre = 8000.0;
    System s;
    s.pal = true;
    s.lockHz = 3.75e6;
    // The pilot on the sync tip.
    s.lockUs = {0.6, 3.4};
    s.hzIre = hzIre;
    s.blankHz = 7.1e6;
    s.syncHz = 7.1e6 - 0.3 * (100.0 / 0.7) * hzIre;
    s.lineUs = 64.0;
    s.coarseBand = {2.6e6, 11.5e6};
    // The pilot's lower sideband sits at 6.76 - 3.75 = 3.0 MHz; the audio carriers (0.68 / 1.07 MHz) and EFM (< 1.75 MHz) stay out.
    s.fineBand = {2.4e6, 12.0e6};
    s.porchUs = {8.3, 9.5};
    s.vbiAfterV = {8.0, 24.0};
    s.vbiLines = {10, 30};
    s.fieldLines = {312, 313};
    return s;
}

// The rate the sample counts below are given at; at another rate each scales to the same duration (`atRate`), so pass 0 measures
// the same thing whatever the capture's rate.
constexpr double kRefFs = 75.0e6;
// Coarse detector: taps and decimation (NTSC; PAL steps by three pilot periods).
constexpr double kCoarseTaps = 48.0;
constexpr double kCoarseStep = 32.0;
// Fine stage taps.
constexpr double kFineTaps = 191.0;
// Boxcar on the fine frequency for the edge and the levels (NTSC).
constexpr double kSmooth = 9.0;
// A line off the straight path of its measured neighbours by more than this is a damaged one.
constexpr double kMaxLineDev = 3.0;
// Samples processed per step, and the context read on each side of them.
constexpr size_t kChunk = size_t(1) << 22;
constexpr size_t kMargin = size_t(1) << 17;
// Lines each field record carries past the next field's first line, as the decoder's do.
constexpr size_t kOverlapLines = 10;
// A vertical interval found within this many lines of where the field structure puts the next field's start is taken as that start.
constexpr size_t kFieldSlack = 3;
// Fields the structure fills in where their vertical intervals were not found (a dropout across them), at most.
constexpr size_t kFieldFill = 4;
// A pulse is never farther than this many lines from the last one on the grid.
constexpr double kMaxSkipLines = 40.0;

double atRate(double n, double fs)
{
    return n * fs / kRefFs;
}

// The same, as a count of at least 1; `odd` keeps a filter symmetric.
size_t countAtRate(double n, double fs, bool odd)
{
    const double r = std::round(atRate(n, fs));
    size_t c = r < 1.0 ? 1 : static_cast<size_t>(r);
    if (odd && c % 2 == 0) {
        ++c;
    }
    return c;
}

// A real as a position in an array, as Rust's `as usize` does it: below zero (and not a number) is 0.
size_t toIndex(double t)
{
    return t > 0.0 ? static_cast<size_t>(t) : 0;
}

double besselI0(double x)
{
    const double x2 = x * x / 4.0;
    double sum = 1.0;
    double term = 1.0;
    double k = 1.0;
    for (;;) {
        term *= x2 / (k * k);
        if (term < sum * 1e-17) {
            return sum;
        }
        sum += term;
        k += 1.0;
    }
}

// A complex band-pass FIR (Kaiser-windowed), as separate re/im taps.
class Fir
{
public:
    Fir(size_t taps, std::pair<double, double> band, double beta, double fs)
    {
        const double half = static_cast<double>(taps - 1) / 2.0;
        const double bw = (band.second - band.first) / fs;
        const double f0 = (band.second + band.first) / 2.0 / fs;
        const double i0b = besselI0(beta);
        m_re.reserve(taps);
        m_im.reserve(taps);
        for (size_t k = 0; k < taps; ++k) {
            const double m = static_cast<double>(k) - half;
            const double x = kPi * bw * m;
            const double sinc = m == 0.0 ? 1.0 : std::sin(x) / x;
            const double r = m / half;
            const double w = besselI0(beta * std::sqrt(std::max(1.0 - r * r, 0.0))) / i0b;
            const double a = bw * sinc * w;
            // Reversed, so the output is a plain dot product with the input.
            const double ph = -2.0 * kPi * f0 * m;
            m_re.push_back(static_cast<float>(a * std::cos(ph)));
            m_im.push_back(static_cast<float>(a * std::sin(ph)));
        }
        // Zero-padded to a multiple of 8 (the dot product's lanes).
        const size_t padded = (taps + 7) / 8 * 8;
        m_re.resize(padded, 0.0f);
        m_im.resize(padded, 0.0f);
        m_half = (taps - 1) / 2;
    }

    size_t half() const { return m_half; }

    // Output centred on x[c] (c >= half, c + half + 8 < n).
    void at(const float *x, size_t c, float &outRe, float &outIm) const
    {
        const float *w = x + (c - m_half);
        float a[8] = {};
        float b[8] = {};
        const size_t n = m_re.size();
        for (size_t i = 0; i < n; i += 8) {
            for (size_t l = 0; l < 8; ++l) {
                a[l] += w[i + l] * m_re[i + l];
                b[l] += w[i + l] * m_im[i + l];
            }
        }
        float sa = 0.0f;
        float sb = 0.0f;
        for (size_t l = 0; l < 8; ++l) {
            sa += a[l];
            sb += b[l];
        }
        outRe = sa;
        outIm = sb;
    }

    // Instantaneous frequency (Hz) at c0..c1 (from the phase step between neighbouring outputs).
    std::vector<float> freq(const float *x, size_t c0, size_t c1, double fs) const
    {
        const float k = static_cast<float>(fs / (2.0 * kPi));
        float pr;
        float pi;
        at(x, c0 - 1, pr, pi);
        std::vector<float> out;
        out.reserve(c1 - c0);
        for (size_t c = c0; c < c1; ++c) {
            float zr;
            float zi;
            at(x, c, zr, zi);
            const float re = zr * pr + zi * pi;
            const float im = zi * pr - zr * pi;
            pr = zr;
            pi = zi;
            out.push_back(std::atan2(im, re) * k);
        }
        return out;
    }

private:
    std::vector<float> m_re;
    std::vector<float> m_im;
    size_t m_half = 0;
};

enum class Kind { H, E, V };

// A sync pulse: absolute start (coarse, samples) and kind.
struct Pulse
{
    double start = 0;
    Kind kind = Kind::H;
};

// The fine measurement of one hsync.
struct Fine
{
    // 50% crossing of the sync's leading edge (absolute samples).
    std::optional<double> edge;
    // Burst phase relative to the edge (radians at fsc).
    std::optional<double> psi;
    // Philips code read on this line, if it is one of the VBI lines looked at.
    std::optional<int64_t> code;
};

using Code = std::vector<int64_t>;

// The VBI words that identify a frame: CAV picture numbers and CLV time codes (top nibble 0xF or 0x8), sorted.
Code frameCode(const std::vector<int64_t> &words)
{
    Code v;
    for (const int64_t w : words) {
        const int64_t top = (w >> 20) & 0xF;
        if (w > 0 && (top == 0x8 || top == 0xF)) {
            v.push_back(w);
        }
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// "Some([a, b])" as Rust prints an Option<Vec<i64>>.
std::string codeText(const Code &c)
{
    std::string s = "Some([";
    for (size_t i = 0; i < c.size(); ++i) {
        s += (i ? ", " : "") + std::to_string(c[i]);
    }
    return s + "])";
}

// The tracking state: the line grid so far and the fields found on it.
class Tracker
{
public:
    Tracker(double fs, const System &sys)
        : m_fs(fs)
        , m_pal(sys.pal)
        , m_lockHz(sys.lockHz)
        , m_vbiLines(sys.vbiLines)
        , m_hlen(fs * sys.lineUs / 1e6)
        , m_fieldLines(sys.fieldLines)
    {
    }

    void pulse(const Pulse &p, const Fine &fine)
    {
        if (p.kind == Kind::H) {
            if (m_lastH) {
                const double ls = m_lastH->second;
                double kr = std::round((p.start - ls) / m_hlen);
                // Off the line grid by more than 0.4 us (plus slack for the line length's drift across a gap, ~0.1 us per line
                // seen): a false pulse (dropout).
                const double off = std::abs(p.start - ls - kr * m_hlen);
                if (kr < 1.0 || off > (0.4 + 0.15 * (kr - 1.0)) * m_fs / 1e6) {
                    // Four pulses off the grid in a row start a new one only if they agree among themselves: in a dropout the
                    // false pulses do not, and the grid holds.
                    ++m_rejects;
                    m_rejected.push_back(p.start);
                    if (m_rejects >= 4 && rejectedOnGrid()) {
                        reset();
                    } else {
                        if (m_rejected.size() >= 4) {
                            m_rejected.erase(m_rejected.begin());
                        }
                        return;
                    }
                } else if (kr > kMaxSkipLines) {
                    reset();
                } else {
                    const size_t n = static_cast<size_t>(kr);
                    if (n == 1) {
                        // Follow the line length (spindle speed).
                        m_hlen += (p.start - ls - m_hlen) / 8.0;
                    }
                    for (size_t i = 1; i < n; ++i) {
                        m_lines.push_back(Fine());
                    }
                }
            }
            m_rejects = 0;
            m_rejected.clear();
            m_lines.push_back(fine);
            m_lastH = std::make_pair(end() - 1, p.start);
            m_inVsync = false;
            m_pending.reset();
            return;
        }
        // An equalizing or a broad pulse.
        if (!m_lastH) {
            return;
        }
        const size_t li = m_lastH->first;
        const double ls = m_lastH->second;
        if (m_inVsync) {
            if (p.kind == Kind::V && m_pending) {
                const std::pair<size_t, bool> found = *m_pending;
                m_pending.reset();
                fieldFound(found.first, found.second);
            }
            return;
        }
        m_inVsync = true;
        if (p.kind != Kind::E) {
            return;
        }
        // The interval opens a full line after the last hsync on one field, half a line after it on the other.
        const double frac = (p.start - ls) / m_hlen;
        bool full;
        if (frac >= 0.85 && frac < 1.15) {
            full = true;
        } else if (frac >= 0.35 && frac < 0.65) {
            full = false;
        } else {
            return;
        }
        const bool first = full != m_pal;
        m_pending = std::make_pair(li + 1, first);
    }

    // Send every field whose lines are all placed.
    void flush(std::vector<Field> &out)
    {
        while (m_fields.size() >= 2) {
            const size_t a = m_fields[0].first;
            const bool first = m_fields[0].second;
            const size_t next = m_fields[1].first;
            const size_t b = next + kOverlapLines;
            // A measured line past the end is needed to interpolate up to it.
            const size_t from = std::min(b > m_base ? b - m_base : 0, m_lines.size());
            bool placed = false;
            for (size_t i = from; i < m_lines.size(); ++i) {
                if (m_lines[i].edge) {
                    placed = true;
                    break;
                }
            }
            if (a < m_base || !placed) {
                if (a < m_base) {
                    m_fields.erase(m_fields.begin());
                    continue;
                }
                return;
            }
            // The field's burst phase reference: circular mean over its lines, modulo half a cycle (consecutive lines alternate
            // by pi).
            double c = 0.0;
            double s = 0.0;
            for (size_t i = a - m_base; i < next - m_base; ++i) {
                if (m_lines[i].psi) {
                    const double p = *m_lines[i].psi;
                    c += std::cos(2.0 * p);
                    s += std::sin(2.0 * p);
                }
            }
            std::optional<double> psi0;
            if (c != 0.0 || s != 0.0) {
                psi0 = std::atan2(s, c) / 2.0;
            }
            std::optional<std::vector<double>> lines = positions(a, b, psi0);
            if (lines) {
                std::optional<Code> code;
                if (first) {
                    std::vector<int64_t> words;
                    for (size_t i = a - m_base + m_vbiLines.first; i < a - m_base + m_vbiLines.second; ++i) {
                        if (i < m_lines.size() && m_lines[i].code) {
                            words.push_back(*m_lines[i].code);
                        }
                    }
                    Code fc = frameCode(words);
                    code = fc.empty() ? std::nullopt : repairCode(std::move(fc));
                } else if (m_lastCode && m_lastCode->first) {
                    code = m_lastCode->second;
                }
                m_lastCode = std::make_pair(first, code);
                Field f;
                f.first = first;
                if (code) {
                    f.code = codeText(*code);
                }
                f.lines = std::move(*lines);
                out.push_back(std::move(f));
            }
            m_fields.erase(m_fields.begin());
            // Keep a few lines before the next field for interpolation.
            const size_t keepFrom = std::max(next > 64 ? next - 64 : size_t(0), m_base);
            m_lines.erase(m_lines.begin(), m_lines.begin() + static_cast<std::ptrdiff_t>(keepFrom - m_base));
            m_base = keepFrom;
        }
    }

private:
    size_t end() const { return m_base + m_lines.size(); }

    // Drop the grid (a jump in the capture): fields found so far that can no longer be completed are lost.
    void reset()
    {
        m_base = end();
        m_lines.clear();
        m_lastH.reset();
        m_fields.clear();
        m_inVsync = false;
        m_lastField.reset();
        m_lock = 0;
        m_rejected.clear();
    }

    // A first field's frame code, with what a dropout over a VBI line took out of it put back: the words the last two codes share
    // (a CLV disc's constant word next to its time code) are added where missing. A code left with only such words identifies no
    // frame (every field has them) and is dropped: the field stays unpaired rather than paired with a wrong one.
    std::optional<Code> repairCode(Code c)
    {
        Code shared;
        if (m_recentCodes.size() == 2) {
            for (const int64_t w : m_recentCodes[0]) {
                if (std::find(m_recentCodes[1].begin(), m_recentCodes[1].end(), w) != m_recentCodes[1].end()) {
                    shared.push_back(w);
                }
            }
        }
        for (const int64_t w : shared) {
            if (std::find(c.begin(), c.end(), w) == c.end()) {
                c.push_back(w);
            }
        }
        std::sort(c.begin(), c.end());
        if (!shared.empty() &&
            std::all_of(c.begin(), c.end(), [&](int64_t w) { return std::find(shared.begin(), shared.end(), w) != shared.end(); })) {
            return std::nullopt;
        }
        m_recentCodes.push_back(c);
        if (m_recentCodes.size() > 2) {
            m_recentCodes.erase(m_recentCodes.begin());
        }
        return c;
    }

    // Lines from the start of a field of parity `first` to the next one.
    size_t fieldLen(bool first) const { return first ? m_fieldLines.first : m_fieldLines.second; }

    // A vertical interval found at line `li`. Once two fields in a row have followed the field structure (alternating parities,
    // fixed lengths), a start within kFieldSlack lines of the next predicted one is taken as that one, with its parity: a dropout
    // over an hsync or an equalizing pulse moves the detection by a line or flips its parity. Missed intervals (up to kFieldFill
    // fields) are filled in, and one found well before the prediction is a false one and dropped. Anything else is a jump in the
    // capture: the structure starts over from it.
    void fieldFound(size_t li, bool first)
    {
        if (m_lastField && m_lock >= 2) {
            const size_t last = m_lastField->first;
            const bool lf = m_lastField->second;
            size_t e = last + fieldLen(lf);
            bool ef = !lf;
            std::vector<std::pair<size_t, bool>> fill;
            while (e + kFieldSlack < li && fill.size() < kFieldFill) {
                fill.emplace_back(e, ef);
                e += fieldLen(ef);
                ef = !ef;
            }
            if (li + kFieldSlack >= e && li <= e + kFieldSlack) {
                m_fields.insert(m_fields.end(), fill.begin(), fill.end());
                m_fields.emplace_back(e, ef);
                m_lastField = std::make_pair(e, ef);
                ++m_lock;
                return;
            }
            if (li + kFieldSlack < last + fieldLen(lf)) {
                return;
            }
        }
        const bool follows = m_lastField && m_lastField->first + fieldLen(m_lastField->second) == li && m_lastField->second != first;
        m_lock = follows ? m_lock + 1 : 1;
        m_fields.emplace_back(li, first);
        m_lastField = std::make_pair(li, first);
    }

    // Whether the rejected pulses lie on a line grid of their own: a jump in the capture, rather than a dropout's noise.
    bool rejectedOnGrid() const
    {
        for (size_t i = 0; i + 1 < m_rejected.size(); ++i) {
            const double d = m_rejected[i + 1] - m_rejected[i];
            const double kr = std::round(d / m_hlen);
            if (!(kr >= 1.0 && kr <= 40.0) || !(std::abs(d - kr * m_hlen) <= (0.4 + 0.15 * (kr - 1.0)) * m_fs / 1e6)) {
                return false;
            }
        }
        return true;
    }

    // Positions of the lines a..b (absolute indices, within the grid): fine where measured, interpolated elsewhere. Nothing when
    // a line at either end cannot be interpolated.
    std::optional<std::vector<double>> positions(size_t a, size_t b, std::optional<double> psi0) const
    {
        const double w = 2.0 * kPi * m_lockHz / m_fs;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::vector<double> pos;
        pos.reserve(m_lines.size());
        for (const Fine &l : m_lines) {
            if (l.edge && l.psi && psi0) {
                // The burst's phase, with its half-cycle ambiguity resolved next to the edge. The correction is the edge's own
                // error, a fraction of a sample: a large one is a damaged burst, and the edge is kept instead.
                double d = *l.psi - *psi0;
                d -= kPi * std::round(d / kPi);
                pos.push_back(std::abs(d / w) < 2.5 ? *l.edge - d / w : *l.edge);
            } else if (l.edge) {
                pos.push_back(*l.edge);
            } else {
                pos.push_back(nan);
            }
        }
        // A line off the straight path of its measured neighbours (the nearest on each side, across unmeasured lines) by more than
        // kMaxLineDev samples is a damaged one (the time base moves far less than that from one line to the next): interpolated
        // instead. Of two adjacent suspects, the worse goes.
        auto neighbours = [&](size_t i) -> std::optional<std::pair<size_t, size_t>> {
            std::optional<size_t> j;
            for (size_t t = i; t-- > 0;) {
                if (std::isfinite(pos[t])) {
                    j = t;
                    break;
                }
            }
            if (!j) {
                return std::nullopt;
            }
            for (size_t k = i + 1; k < pos.size(); ++k) {
                if (std::isfinite(pos[k])) {
                    return std::make_pair(*j, k);
                }
            }
            return std::nullopt;
        };
        auto dev = [&](size_t i) -> double {
            if (!std::isfinite(pos[i])) {
                return 0.0;
            }
            const auto nb = neighbours(i);
            if (!nb) {
                return 0.0;
            }
            const size_t j = nb->first;
            const size_t k = nb->second;
            return std::abs(pos[i] - (pos[j] + (pos[k] - pos[j]) * static_cast<double>(i - j) / static_cast<double>(k - j)));
        };
        const double maxDev = atRate(kMaxLineDev, m_fs);
        std::vector<size_t> bad;
        for (size_t i = 0; i < pos.size(); ++i) {
            const double d = dev(i);
            if (d > maxDev) {
                const auto nb = neighbours(i);
                if (nb && dev(nb->first) <= d && dev(nb->second) <= d) {
                    bad.push_back(i);
                }
            }
        }
        for (const size_t i : bad) {
            pos[i] = nan;
        }
        if (a < m_base || b < m_base) {
            return std::nullopt;
        }
        const size_t ra = a - m_base;
        const size_t rb = b - m_base;
        std::vector<double> out;
        out.reserve(rb > ra ? rb - ra : 0);
        for (size_t i = ra; i < rb; ++i) {
            if (i < pos.size() && std::isfinite(pos[i])) {
                out.push_back(pos[i]);
                continue;
            }
            std::optional<size_t> lo;
            for (size_t t = std::min(i, pos.size()); t-- > 0;) {
                if (std::isfinite(pos[t])) {
                    lo = t;
                    break;
                }
            }
            std::optional<size_t> hi;
            for (size_t t = i + 1; t < pos.size(); ++t) {
                if (std::isfinite(pos[t])) {
                    hi = t;
                    break;
                }
            }
            if (!lo || !hi) {
                return std::nullopt;
            }
            const double t = static_cast<double>(i - *lo) / static_cast<double>(*hi - *lo);
            out.push_back(pos[*lo] + t * (pos[*hi] - pos[*lo]));
        }
        return out;
    }

    double m_fs;
    // PAL: the half-line start of the vertical interval marks the first field (NTSC: the full-line one).
    bool m_pal;
    double m_lockHz;
    std::pair<size_t, size_t> m_vbiLines;
    double m_hlen;
    // Absolute index of m_lines[0].
    size_t m_base = 0;
    // The grid's lines; a line with no hsync (vertical interval, dropout) has no edge and is interpolated.
    std::vector<Fine> m_lines;
    // Absolute index and coarse position of the last line with an hsync.
    std::optional<std::pair<size_t, double>> m_lastH;
    size_t m_rejects = 0;
    bool m_inVsync = false;
    // A field start seen on its first equalizing pulse, kept once a broad pulse confirms the vertical interval (a lone short pulse,
    // such as an hsync broken up by a dropout or by the PAL pilot, is not one).
    std::optional<std::pair<size_t, bool>> m_pending;
    // Fields found (absolute first-line index, first field) and not yet sent.
    std::vector<std::pair<size_t, bool>> m_fields;
    // Frame code of the last first field sent (a second field inherits it).
    std::optional<std::pair<bool, std::optional<Code>>> m_lastCode;
    std::pair<size_t, size_t> m_fieldLines;
    // The last field found (absolute first line, first field), and how many fields in a row have followed the field structure up to it.
    std::optional<std::pair<size_t, bool>> m_lastField;
    size_t m_lock = 0;
    // Pulses off the line grid since the last one on it.
    std::vector<double> m_rejected;
    // The last two first fields' frame codes that identified a frame.
    std::vector<Code> m_recentCodes;
};

// Pulses in `freq` (the coarse frequency at t0 + i * step): runs below `thr`, timed by linear interpolation of the crossings.
std::vector<Pulse> findPulses(const std::vector<float> &freq, double t0, size_t stepSamples, float thr, double fs)
{
    const double step = static_cast<double>(stepSamples);
    const double us = fs / 1e6;
    std::vector<Pulse> out;
    std::optional<double> start;
    for (size_t i = 1; i < freq.size(); ++i) {
        const float a = freq[i - 1];
        const float b = freq[i];
        const double t = t0 + static_cast<double>(i - 1) * step + step * static_cast<double>((a - thr) / (a - b));
        if (a >= thr && b < thr) {
            start = t;
        } else if (a < thr && b >= thr) {
            if (!start) {
                continue;
            }
            const double s = *start;
            start.reset();
            const double d = (t - s) / us;
            Kind kind;
            if (d >= 3.6 && d < 6.2) {
                kind = Kind::H;
            } else if (d >= 1.6 && d < 3.2) {
                kind = Kind::E;
            } else if (d >= 15.0 && d < 40.0) {
                kind = Kind::V;
            } else {
                continue;
            }
            out.push_back({s, kind});
        }
    }
    return out;
}

// Median of a small slice (sorted).
float median(std::vector<float> v)
{
    std::sort(v.begin(), v.end(), [](float x, float y) {
        // total order, as Rust's f32::total_cmp
        int32_t a;
        int32_t b;
        std::memcpy(&a, &x, 4);
        std::memcpy(&b, &y, 4);
        a ^= static_cast<int32_t>(static_cast<uint32_t>(a >> 31) >> 1);
        b ^= static_cast<int32_t>(static_cast<uint32_t>(b >> 31) >> 1);
        return a < b;
    });
    return v[v.size() / 2];
}

// A Philips code (24 bits, biphase, 2 us per bit) on the line starting at `edge`, read as the decoder reads it: transitions through
// 50 IRE.
std::optional<int64_t> philips(const std::vector<float> &sm, size_t c0, double edge, float porch, double fs, double hzIre)
{
    const double us = fs / 1e6;
    const float thr = porch + static_cast<float>(50.0 * hzIre);
    auto at = [&](double t) { return toIndex(t - static_cast<double>(c0)); };
    auto cross = [&](double from, double len) -> std::optional<double> {
        const size_t a = std::max<size_t>(at(from), 1);
        const size_t b = std::min(at(from + len), sm.size());
        for (size_t i = a; i < b; ++i) {
            if ((sm[i - 1] < thr) != (sm[i] < thr)) {
                return static_cast<double>(c0) + static_cast<double>(i - 1) + static_cast<double>((sm[i - 1] - thr) / (sm[i - 1] - sm[i]));
            }
        }
        return std::nullopt;
    };
    std::vector<std::pair<double, bool>> zc;
    zc.reserve(25);
    std::optional<double> cur = cross(edge + 2.0 * us, 12.0 * us);
    while (cur) {
        const double c = *cur;
        const size_t i = at(c - 0.5 * us);
        zc.emplace_back(c, i < sm.size() && sm[i] < thr);
        if (zc.size() > 24) {
            return std::nullopt;
        }
        // Next mid-bit transition 2 us on (boundary ones are 1 us away). The window is wider than the decoder's 1.9..2.1: on the RF
        // the rising and falling edges sit ~0.1 us apart.
        cur = cross(c + 1.75 * us, 0.5 * us);
    }
    if (zc.size() != 24) {
        return std::nullopt;
    }
    for (size_t i = 0; i + 1 < zc.size(); ++i) {
        const double d = (zc[i + 1].first - zc[i].first) / us;
        if (!(d >= 1.75 && d < 2.25)) {
            return std::nullopt;
        }
    }
    int64_t v = 0;
    for (const auto &z : zc) {
        v = v << 1 | static_cast<int64_t>(z.second);
    }
    return v;
}

// Refine one hsync whose coarse start is `s` (index into `x`).
Fine refine(const std::vector<float> &x, double s, const Fir &fir, double fs, bool vbi, const System &sys)
{
    const double us = fs / 1e6;
    const size_t half = fir.half();
    const double c0d = std::trunc(s - 1.2 * us);
    // Up to the end of the porch window as seen from an edge up to 1.5 us late (the tip search), plus the average's reach: every
    // sample read below, and no more (this is the bulk of pass 0's work).
    const double span = vbi ? 64.0 : sys.porchUs.second + 1.6;
    const double c1d = std::trunc(s + span * us);
    if (c0d < static_cast<double>(half) + 1.0 || static_cast<size_t>(c1d) + half + 8 >= x.size()) {
        return Fine();
    }
    const size_t c0 = static_cast<size_t>(c0d);
    const size_t c1 = static_cast<size_t>(c1d);
    const std::vector<float> f = fir.freq(x.data(), c0, c1, fs);
    // Centred boxcar for the edge and the levels: kSmooth samples, or three pilot periods on PAL (a whole number at 40 MHz, so it
    // nulls the pilot; one rounded period leaves a ripple that crosses the threshold at the start of the sync tip). The edge, blurred
    // symmetrically, keeps its 50% point.
    const size_t len = sys.pal ? static_cast<size_t>(std::round(3.0 * fs / sys.lockHz)) : countAtRate(kSmooth, fs, false);
    std::vector<float> sm(f.size());
    for (size_t i = 0; i < f.size(); ++i) {
        const size_t a = i > len / 2 ? i - len / 2 : 0;
        const size_t b = std::min(i + len - len / 2, f.size());
        float sum = 0.0f;
        for (size_t t = a; t < b; ++t) {
            sum += f[t];
        }
        sm[i] = sum / static_cast<float>(b - a);
    }
    auto at = [&](double t) { return toIndex(t - static_cast<double>(c0)); };
    // A slice of sm, or nothing where Rust would have panicked (the pulse is then left out).
    auto slice = [&](double a, double b, std::vector<float> &out) {
        const size_t ia = at(a);
        const size_t ib = at(b);
        if (ia > ib || ib > sm.size()) {
            return false;
        }
        out.assign(sm.begin() + static_cast<std::ptrdiff_t>(ia), sm.begin() + static_cast<std::ptrdiff_t>(ib));
        return !out.empty();
    };
    const double hzIre = sys.hzIre;
    std::vector<float> part;
    if (!slice(s + 1.0 * us, s + 2.5 * us, part)) {
        return Fine();
    }
    const float sync = median(part);
    if (!slice(s + sys.porchUs.first * us, s + sys.porchUs.second * us, part)) {
        return Fine();
    }
    const float porch = median(part);
    const double depth = static_cast<double>(porch - sync) / hzIre;
    if (!(depth >= 25.0 && depth < 55.0)) {
        return Fine();
    }
    const float mid = (porch + sync) / 2.0f;
    // The leading edge: the crossing closest to the sync tip, walking back from inside it. A dark-going transition just before the
    // front porch undershoots through the threshold on the still pre-emphasized RF (PAL especially); a forward search would stop there.
    const size_t from = std::max<size_t>(at(s - 1.0 * us), 1);
    const size_t tip = at(s + 1.5 * us);
    if (tip >= sm.size() || sm[tip] >= mid) {
        return Fine();
    }
    std::optional<size_t> found;
    for (size_t i = tip + 1; i-- > from;) {
        if (sm[i - 1] >= mid && sm[i] < mid) {
            found = i;
            break;
        }
    }
    if (!found) {
        return Fine();
    }
    const size_t fi = *found;
    const double edge = static_cast<double>(c0) + static_cast<double>(fi - 1) + static_cast<double>((sm[fi - 1] - mid) / (sm[fi - 1] - sm[fi]));
    // Dropout check: the pulse and the porch stay within -90..50 IRE. Wider than the decoder's -55..30, since on the RF the video is
    // still pre-emphasized (the edges overshoot); a dropout sends the carrier much further off. The burst is left out (pre-emphasized
    // as well).
    const float lo = porch - 90.0f * static_cast<float>(hzIre);
    const float hi = porch + 50.0f * static_cast<float>(hzIre);
    auto ok = [&](double a, double b) {
        const size_t ia = std::min(at(a), sm.size());
        const size_t ib = std::min(at(b), sm.size());
        for (size_t i = ia; i < ib; ++i) {
            if (!(sm[i] >= lo && sm[i] <= hi)) {
                return false;
            }
        }
        return true;
    };
    if (!ok(edge - 0.75 * us, edge + 4.0 * us) || !ok(edge + sys.porchUs.first * us, edge + sys.porchUs.second * us)) {
        return Fine();
    }
    // Burst (NTSC) or pilot (PAL): least-squares phase over its window.
    const double w = 2.0 * kPi * sys.lockHz / fs;
    const double b0 = edge + sys.lockUs.first * us;
    const double b1 = edge + sys.lockUs.second * us;
    const size_t i0 = at(b0);
    const size_t i1 = std::min(at(b1), f.size());
    if (i0 >= i1) {
        return Fine();
    }
    double mean = 0.0;
    for (size_t i = i0; i < i1; ++i) {
        mean += static_cast<double>(f[i]);
    }
    mean /= static_cast<double>(i1 - i0);
    double pr = 0.0;
    double pi = 0.0;
    for (size_t k = 0; i0 + k < i1; ++k) {
        const double ph = w * (static_cast<double>(c0 + i0 + k) - edge);
        const double v = static_cast<double>(f[i0 + k]) - mean;
        pr += v * std::cos(ph);
        pi -= v * std::sin(ph);
    }
    const double amp = 2.0 * std::sqrt(pr * pr + pi * pi) / static_cast<double>(i1 - i0) / hzIre;
    std::optional<double> psi;
    if (amp > 5.0) {
        psi = std::atan2(pi, pr);
    }
    // PAL: the code is read on the de-emphasized frequency (100 / 400 ns, bilinear), lightly averaged: on the pre-emphasized RF its
    // transitions overshoot, and the sync tip's three-period average blurs its 1 us half-cells.
    std::optional<int64_t> code;
    if (!vbi) {
        code = std::nullopt;
    } else if (!sys.pal) {
        code = philips(sm, c0, edge, porch, fs, hzIre);
    } else {
        const double t1 = 100e-9;
        const double t2 = 400e-9;
        const double k = 2.0 * fs;
        const double bb0 = (1.0 + k * t1) / (1.0 + k * t2);
        const double bb1 = (1.0 - k * t1) / (1.0 + k * t2);
        const double aa1 = (1.0 - k * t2) / (1.0 + k * t2);
        std::vector<float> de;
        de.reserve(f.size());
        double x1 = static_cast<double>(porch);
        double y1 = static_cast<double>(porch);
        for (const float v : f) {
            const double y = bb0 * static_cast<double>(v) + bb1 * x1 - aa1 * y1;
            de.push_back(static_cast<float>(y));
            x1 = static_cast<double>(v);
            y1 = y;
        }
        std::vector<float> light(de.size());
        for (size_t i = 0; i < de.size(); ++i) {
            const size_t a = i > 4 ? i - 4 : 0;
            const size_t b = std::min(i + 5, de.size());
            float sum = 0.0f;
            for (size_t t = a; t < b; ++t) {
                sum += de[t];
            }
            light[i] = sum / static_cast<float>(b - a);
        }
        code = philips(light, c0, edge, porch, fs, hzIre);
    }
    Fine out;
    out.edge = edge;
    out.psi = psi;
    out.code = code;
    return out;
}

} // namespace

std::vector<Field> locateFields(TrackReader &reader, size_t track, double fs, VideoSystem video, unsigned threads, const std::atomic<bool> *stop, const std::function<void(double)> &progress)
{
    if (!(fs > 0.0)) {
        throw Error("the TBC curve (pass 0) needs the sample rate of the capture");
    }
    if (threads == 0) {
        threads = defaultThreads();
    }
    const System sys = video == VideoSystem::Pal ? pal() : ntsc();
    const Fir coarse(countAtRate(kCoarseTaps, fs, false), sys.coarseBand, 6.0, fs);
    const Fir fine(countAtRate(kFineTaps, fs, true), sys.fineBand, 7.0, fs);
    const float thr = static_cast<float>((sys.syncHz + sys.blankHz) / 2.0);
    const double hlen = fs * sys.lineUs / 1e6;
    // PAL: a step of three pilot periods, the phase steps summed at eight points spread over it, so the pilot averages out.
    size_t step;
    std::vector<size_t> points;
    if (sys.pal) {
        step = static_cast<size_t>(std::round(3.0 * fs / sys.lockHz));
        for (size_t k = 0; k < 8; ++k) {
            points.push_back(k * step / 8);
        }
    } else {
        step = countAtRate(kCoarseStep, fs, false);
        points.push_back(0);
    }
    const size_t reach = points.back();
    const uint64_t length = reader.meta().sources.at(track).words;
    const float scale = reader.meta().sources.at(track).fmt == "s8" ? 256.0f : 1.0f;
    Tracker tr(fs, sys);
    std::vector<Field> fields;
    std::vector<float> buf(kChunk + 2 * kMargin);
    std::vector<int32_t> raw(buf.size());
    uint64_t own0 = 0;
    while (own0 < length) {
        if (stop && stop->load()) {
            break;
        }
        const int64_t b0 = static_cast<int64_t>(own0) - static_cast<int64_t>(kMargin);
        // The samples as ld-decode reads them: zeros outside the capture.
        reader.readOnce(track, b0, buf.size(), raw.data());
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = static_cast<float>(raw[i]) * scale;
        }
        // Coarse frequency on a grid aligned to absolute multiples of the step, so chunk boundaries do not move it.
        const size_t h = coarse.half();
        const int64_t first = std::max<int64_t>(b0 + static_cast<int64_t>(h) + 1, 0);
        const uint64_t g0 = (static_cast<uint64_t>(first) + step - 1) / step * step;
        const int64_t limit = b0 + static_cast<int64_t>(buf.size()) - static_cast<int64_t>(h) - 10 - static_cast<int64_t>(reach);
        const size_t n = limit > static_cast<int64_t>(g0) ? static_cast<size_t>(limit - static_cast<int64_t>(g0)) / step : 0;
        const float k = static_cast<float>(fs / (2.0 * kPi));
        std::vector<std::pair<float, float>> prods(n);
        parallelFor(n, threads, [&](size_t j) {
            const size_t c = static_cast<size_t>(static_cast<int64_t>(g0) + static_cast<int64_t>(j * step) - b0);
            float accRe = 0.0f;
            float accIm = 0.0f;
            for (const size_t o : points) {
                float ar;
                float ai;
                float br;
                float bi;
                coarse.at(buf.data(), c + o, ar, ai);
                coarse.at(buf.data(), c + o + 1, br, bi);
                accRe = accRe + br * ar + bi * ai;
                accIm = accIm + bi * ar - br * ai;
            }
            prods[j] = {accRe, accIm};
        });
        std::vector<float> freq(prods.size() > 0 ? prods.size() - 1 : 0);
        for (size_t j = 0; j < freq.size(); ++j) {
            const float re = prods[j].first + prods[j + 1].first;
            const float im = prods[j].second + prods[j + 1].second;
            freq[j] = std::atan2(im, re) * k;
        }
        const double t0 = static_cast<double>(g0) + static_cast<double>(step) / 2.0;
        const std::vector<Pulse> pulses = findPulses(freq, t0, step, thr, fs);
        const uint64_t own1 = std::min<uint64_t>(own0 + kChunk, length);
        // VBI candidates: hsyncs 6.5 to 13.5 lines after a broad pulse.
        double lastV = -std::numeric_limits<double>::infinity();
        std::vector<std::pair<Pulse, bool>> jobs;
        for (const Pulse &p : pulses) {
            if (p.kind == Kind::V) {
                lastV = p.start;
            }
            const bool owned = p.start >= static_cast<double>(own0) && p.start < static_cast<double>(own1);
            const double lines = (p.start - lastV) / hlen;
            const bool vbi = p.kind == Kind::H && lines >= sys.vbiAfterV.first && lines < sys.vbiAfterV.second;
            if (owned) {
                jobs.emplace_back(p, vbi);
            }
        }
        std::vector<Fine> fines(jobs.size());
        parallelFor(jobs.size(), threads, [&](size_t j) {
            const Pulse &p = jobs[j].first;
            if (p.kind != Kind::H) {
                return;
            }
            Fine f = refine(buf, p.start - static_cast<double>(b0), fine, fs, jobs[j].second, sys);
            if (f.edge) {
                f.edge = *f.edge + static_cast<double>(b0);
            }
            fines[j] = f;
        });
        for (size_t j = 0; j < jobs.size(); ++j) {
            tr.pulse(jobs[j].first, fines[j]);
        }
        tr.flush(fields);
        own0 = own1;
        if (progress) {
            progress(length > 0 ? static_cast<double>(own0) / static_cast<double>(length) : 1.0);
        }
    }
    return fields;
}

std::optional<VideoSystem> systemOfStandard(const std::string &standard)
{
    if (standard == "pal" || standard == "secam") {
        return VideoSystem::Pal;
    }
    if (standard == "ntsc" || standard == "pal-m" || standard == "ntsc-vhd") {
        return VideoSystem::Ntsc;
    }
    return std::nullopt;
}

std::array<VideoSystem, 2> systemsToTry(const std::string &standard)
{
    if (systemOfStandard(standard) == VideoSystem::Pal) {
        return {VideoSystem::Pal, VideoSystem::Ntsc};
    }
    return {VideoSystem::Ntsc, VideoSystem::Pal};
}

std::optional<std::string> doubtedStandard(const std::string &standard, bool foundPal)
{
    const std::optional<VideoSystem> said = systemOfStandard(standard);
    if (!said || (*said == VideoSystem::Pal) == foundPal) {
        return std::nullopt;
    }
    return foundPal ? "pal" : "ntsc";
}

double nominalLineSamples(double fs, VideoSystem system)
{
    return fs * (system == VideoSystem::Pal ? pal() : ntsc()).lineUs / 1e6;
}

size_t keepPass0(const QString &archive, double fallbackRateMhz, bool onlyMissing, const std::atomic<bool> *stop, std::optional<size_t> track)
{
    TrackReader reader(archive);
    if (!reader.keepsSpeed()) {
        throw Error(archive.toStdString() + ": only a predictor archive can keep a TBC curve");
    }
    const size_t count = reader.meta().sources.size();
    if (track && *track >= count) {
        throw Error(archive.toStdString() + ": the archive has " + std::to_string(count) + " tracks");
    }
    std::map<size_t, SavedPass0> found;
    // What the archive has already is kept as it is.
    std::vector<size_t> todo;
    for (size_t k = 0; k < count; ++k) {
        const SavedPass0 *have = reader.pass0(k);
        if (have) {
            found[k] = *have;
        }
        if ((!have || !onlyMissing) && (!track || *track == k)) {
            todo.push_back(k);
        }
    }
    if (todo.empty()) {
        return 0;
    }
    std::mutex mutex;
    std::vector<int> percent(count, 0);
    size_t finished = 0;
    std::string failure;
    const auto report = [&](bool last) {
        int sum = 0;
        for (const size_t k : todo) {
            sum += percent[k];
        }
        const int mean = sum / static_cast<int>(todo.size());
        std::fprintf(stderr, "pass 0 [%zu/%zu] curve: %d%%%c", finished, todo.size(), mean, last ? '\n' : '\r');
        std::fflush(stderr);
    };
    std::fprintf(stderr, "working out the TBC curve of %zu track(s) (pass 0)\n", todo.size());
    std::vector<std::thread> workers;
    for (const size_t k : todo) {
        workers.emplace_back([&, k] {
            try {
                const double rate = reader.meta().sources[k].rateHz > 0 ? reader.meta().sources[k].rateHz : fallbackRateMhz * 1e6;
                // (the standard said of the track tells which system to try first)
                for (const VideoSystem system : systemsToTry(reader.meta().sources[k].standard)) {
                    const bool pal = system == VideoSystem::Pal;
                    std::vector<Field> fields = locateFields(reader, k, rate, system, 0, stop, [&](double part) {
                        const int now = std::clamp(static_cast<int>(part * 100.0), 0, 100);
                        std::lock_guard<std::mutex> lock(mutex);
                        if (now != percent[k]) {
                            percent[k] = now;
                            report(false);
                        }
                    });
                    const bool usable = std::any_of(fields.begin(), fields.end(), [](const Field &f) { return f.lines.size() >= 20; });
                    if (usable || (stop && stop->load())) {
                        if (usable) {
                            std::lock_guard<std::mutex> lock(mutex);
                            found[k] = SavedPass0{pal, rate, std::move(fields)};
                        }
                        break;
                    }
                }
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lock(mutex);
                failure = e.what();
            }
            std::lock_guard<std::mutex> lock(mutex);
            percent[k] = 100;
            ++finished;
            report(finished == todo.size());
        });
    }
    for (std::thread &w : workers) {
        w.join();
    }
    if (stop && stop->load()) {
        throw Error("stopped");
    }
    if (!failure.empty()) {
        throw Error(failure);
    }
    size_t done = 0;
    for (const size_t k : todo) {
        done += found.count(k) && (!reader.pass0(k) || !onlyMissing) ? 1 : 0;
    }
    savePass0(archive, found);
    return done;
}

std::string formatPass0(const std::vector<Field> &fields)
{
    std::string s;
    for (const Field &f : fields) {
        s += f.first ? "true " : "false ";
        s += f.code ? *f.code : std::string("None");
        for (const double l : f.lines) {
            uint64_t bits;
            std::memcpy(&bits, &l, sizeof(bits));
            char buf[24];
            std::snprintf(buf, sizeof(buf), " %016llx", static_cast<unsigned long long>(bits));
            s += buf;
        }
        s += '\n';
    }
    return s;
}

} // namespace rfx
