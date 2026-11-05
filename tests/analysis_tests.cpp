/******************************************************************************
 * analysis_tests.cpp
 * rfx library - reading the tracks of an .rfx file at any place, and looking at a
 * stretch of samples (spectrum, noise, lag)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "analysis.h"
#include "error.h"
#include "capture.h"
#include "ffmpeg.h"
#include "flacmode.h"
#include "packer.h"
#include "posfile.h"
#include "rfsync.h"
#include "sampleformat.h"
#include "trackreader.h"

#include <optional>

#include <QProcess>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

struct Lcg
{
    uint32_t x;
    uint32_t next()
    {
        x = x * 1664525u + 1013904223u;
        return x;
    }
};

double uniform(Lcg &l)
{
    return static_cast<double>(l.next() >> 8) / 16777216.0 - 0.5;
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        std::fprintf(stderr, "cannot write %s\n", path.toStdString().c_str());
        ++failures;
    }
}

// Tones and some noise as 16-bit samples shifted left by 6 bits, starting `lag`
// samples into the signal.
QByteArray capture(size_t samples, int lag, uint32_t seed)
{
    QByteArray out(static_cast<qsizetype>(samples * 2), 0);
    Lcg noise{seed};
    for (size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(static_cast<int64_t>(i) + lag);
        const double v = 180 * std::sin(t * 0.21) + 120 * std::sin(t * 0.0137 + 1) + 60 * std::sin(t * 0.77);
        const int tenBit = static_cast<int>(std::lround(v)) + static_cast<int>(noise.next() >> 29) - 3;
        const int16_t s = static_cast<int16_t>(tenBit * 64);
        std::memcpy(out.data() + 2 * i, &s, 2);
    }
    return out;
}

QByteArray pass0Dump(size_t frames, size_t lines, double spacing, double first)
{
    std::string text;
    const double fieldLength = static_cast<double>(lines) * spacing;
    for (size_t f = 0; f < frames; ++f) {
        for (int half = 0; half < 2; ++half) {
            text += half == 0 ? "true " : "false ";
            text += "Some([" + std::to_string(f + 1000) + ", 0])";
            const double start = first + (static_cast<double>(f) * 2 + half) * fieldLength;
            for (size_t i = 0; i < lines; ++i) {
                const double pos = start + static_cast<double>(i) * spacing;
                uint64_t bits;
                std::memcpy(&bits, &pos, sizeof(bits));
                char buf[24];
                std::snprintf(buf, sizeof(buf), " %llx", static_cast<unsigned long long>(bits));
                text += buf;
            }
            text += "\n";
        }
    }
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

void testSpectrum()
{
    const size_t fft = 1024;
    const double rate = 1024.0;
    // A sine on the bin 100 with an amplitude of 100 over noise of 3 rms.
    std::vector<double> x(16384);
    Lcg l{7};
    for (size_t i = 0; i < x.size(); ++i) {
        double n = 0;
        for (int k = 0; k < 12; ++k) {
            n += uniform(l);
        }
        x[i] = 100.0 * std::sin(2.0 * 3.14159265358979 * 100.0 * static_cast<double>(i) / static_cast<double>(fft)) + 3.0 * n;
    }
    const rfx::Spectrum s = rfx::analyseSpectrum(x, fft, rate, 512.0);
    CHECK(s.power.size() == fft / 2 + 1);
    CHECK(s.frames > 1);
    size_t peak = 0;
    for (size_t b = 1; b < s.power.size(); ++b) {
        if (s.power[b] > s.power[peak]) {
            peak = b;
        }
    }
    CHECK(peak == 100);
    // 100 against a full scale of 512: about -14 dB.
    CHECK(std::fabs(s.db[100] - 20.0 * std::log10(100.0 / 512.0)) < 1.0);

    // A band that holds the sine: the floor is still the noise, and the mean of
    // the band stands far above it.
    const rfx::SampleStats st = rfx::computeStats(x, s, 120.0);
    CHECK(st.noiseFloor > 2.7 && st.noiseFloor < 3.3);
    CHECK(st.snrDb > 25.0);
    CHECK(st.meanAmplitude > 50.0 && st.meanAmplitude < 80.0);
    CHECK(std::fabs(st.effectiveBits - (st.snrDb - 1.76) / 6.02) < 1e-9);

    // White noise alone: a signal-to-noise ratio of about nothing.
    for (double &v : x) {
        double n = 0;
        for (int k = 0; k < 12; ++k) {
            n += uniform(l);
        }
        v = 3.0 * n;
    }
    const rfx::SampleStats noise = rfx::computeStats(x, rfx::analyseSpectrum(x, fft, rate, 512.0), 400.0);
    CHECK(noise.noiseFloor > 2.7 && noise.noiseFloor < 3.3);
    CHECK(std::fabs(noise.snrDb) < 1.0);

    bool refused = false;
    try {
        rfx::analyseSpectrum(x, 1000, rate, 512.0);
    } catch (const rfx::Error &) {
        refused = true;
    }
    CHECK(refused);

    CHECK(rfx::carriedBits(rfx::SampleFormat::S16, 6) == 10);
    CHECK(rfx::carriedBits(rfx::SampleFormat::S8, 0) == 8);
    CHECK(std::fabs(rfx::toConverterUnits(rfx::SampleFormat::S16, 64 * 100, 10) - 100.0) < 1e-9);
    CHECK(std::fabs(rfx::defaultBandHighHz(75e6) - 14e6) < 1.0 && std::fabs(rfx::defaultBandHighHz(30e6) - 13.5e6) < 1.0);
}

void testReader()
{
    QTemporaryDir dir;
    const size_t samples = 1200000;
    const QByteArray a = capture(samples, 0, 1);
    const QByteArray b = capture(samples, 5, 2);
    writeFile(dir.filePath("a.ddd.s16"), a);
    writeFile(dir.filePath("b.ddd.s16"), b);
    QDir().mkpath(dir.filePath("p0"));
    writeFile(dir.filePath("p0/pass0_0.txt"), pass0Dump(30, 20, 1000, 500));
    writeFile(dir.filePath("p0/pass0_1.txt"), pass0Dump(30, 20, 1000, 495));
    rfx::PackOptions pack;
    pack.threads = 2;
    pack.order = rfx::OrderMode::Given;
    pack.segment = 65536;
    pack.pass0Dir = dir.filePath("p0");
    rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, dir.filePath("two.rfx"), pack);

    rfx::TrackReader reader(dir.filePath("two.rfx"));
    CHECK(reader.trackCount() == 2);
    CHECK(reader.meta().sources[1].refs == std::vector<size_t>{0});
    CHECK(reader.lagTable(1, 0) != nullptr && reader.lagTable(0, 1) == nullptr);
    CHECK(reader.packedBytes(0) > 0 && reader.packedBytes(1) < reader.packedBytes(0));
    CHECK(reader.fileSize() > 0);

    // Windows of the second track, which is predicted from the first: inside a
    // segment, across a boundary, over the start and the end, and far away.
    struct Case
    {
        int64_t start;
        size_t n;
    };
    const Case cases[] = {{1000, 500}, {65000, 2000}, {131072 - 10, 20}, {-300, 700}, {static_cast<int64_t>(samples) - 400, 900}, {900000, 70000}, {5000000, 10}};
    for (size_t k = 0; k < 2; ++k) {
        const QByteArray &source = k == 0 ? a : b;
        for (const Case &c : cases) {
            std::vector<int32_t> got(c.n);
            reader.read(k, c.start, c.n, got.data());
            bool same = true;
            for (size_t i = 0; i < c.n; ++i) {
                const int64_t p = c.start + static_cast<int64_t>(i);
                int16_t want = 0;
                if (p >= 0 && p < static_cast<int64_t>(samples)) {
                    std::memcpy(&want, source.constData() + 2 * p, 2);
                }
                same = same && got[i] == want;
            }
            CHECK(same);
        }
    }

    // A file that is not an archive is refused.
    writeFile(dir.filePath("not.rfx"), QByteArray("not an archive"));
    bool refused = false;
    try {
        rfx::TrackReader bad(dir.filePath("not.rfx"));
    } catch (const rfx::Error &) {
        refused = true;
    }
    CHECK(refused);

    // The lag series of the table is its step function.
    const rfx::LagTable *table = reader.lagTable(1, 0);
    if (table) {
        const std::vector<int64_t> series = rfx::lagSeries(*table, samples, 9);
        CHECK(series.size() == 9);
        CHECK(series.front() == table->lagAt(0));
        CHECK(series.back() == table->lagAt(static_cast<int64_t>(samples) - 1));
    }
}

// The speed is the slope of the lag, in parts per million, positive when it grows.
void testSpeed()
{
    // A lag that grows by a sample every thousand: a thousand parts per million
    // faster; one that falls twice as fast: two thousand slower.
    rfx::LagTable up;
    rfx::LagTable down;
    for (uint64_t p = 0; p <= 100000; p += 1000) {
        up.pos.push_back(p);
        up.lag.push_back(static_cast<int64_t>(p / 1000));
        down.pos.push_back(p);
        down.lag.push_back(-static_cast<int64_t>(p / 500));
    }
    const std::vector<double> faster = rfx::speedDeviationPpm(up, 100001, 11, 20000);
    const std::vector<double> slower = rfx::speedDeviationPpm(down, 100001, 11, 20000);
    CHECK(faster.size() == 11);
    bool sure = true;
    for (size_t i = 0; i < faster.size(); ++i) {
        sure = sure && std::fabs(faster[i] - 1000.0) < 60.0 && std::fabs(slower[i] + 2000.0) < 120.0;
    }
    CHECK(sure);

    // Flat: no deviation, whatever the lag. A jump shows as a burst where it is, and
    // as nothing far from it.
    rfx::LagTable flat;
    flat.pos = {0};
    flat.lag = {7};
    for (const double v : rfx::speedDeviationPpm(flat, 50000, 5, 4000)) {
        CHECK(v == 0.0);
    }
    rfx::LagTable jump;
    jump.pos = {0, 25000};
    jump.lag = {0, 3};
    const std::vector<double> burst = rfx::speedDeviationPpm(jump, 50001, 51, 2000);
    CHECK(burst[25] > 1000.0);
    CHECK(burst[0] == 0.0 && burst[50] == 0.0 && burst[10] == 0.0);

    // Nothing to measure over.
    CHECK(rfx::speedDeviationPpm(flat, 1, 4, 100) == std::vector<double>(4, 0.0));
    CHECK(rfx::speedDeviationPpm(flat, 1000, 0, 100).empty());
}

// A raw capture is read where it is, in any of its formats, with the bits it carries
// found from its samples or given.
void testCaptureReader()
{
    QTemporaryDir dir;
    const size_t samples = 100003;
    // Ten-bit values on the 16-bit scale: whole multiples of 64, as the Duplicator's.
    std::vector<int32_t> values(samples);
    Lcg l{5};
    for (size_t i = 0; i < samples; ++i) {
        values[i] = (static_cast<int32_t>(l.next() >> 22) - 512) * 64;
    }

    // .lds: packed groups of four, written through the library's own writer.
    {
        rfx::PosFile f(dir.filePath("a.lds"), rfx::PosFile::Mode::Write);
        const size_t whole = samples / 4 * 4;
        rfx::storeSamples(rfx::SampleFormat::Lds, f, 0, values.data(), whole);
        rfx::CaptureSpec spec;
        spec.format = rfx::SampleFormat::Lds;
        spec.rateHz = 40e6;
        rfx::TrackReader r(dir.filePath("a.lds"), spec);
        CHECK(!r.isArchive());
        CHECK(r.trackCount() == 1);
        CHECK(r.meta().sources[0].words == whole);
        CHECK(r.meta().sources[0].shift == 6);
        CHECK(r.meta().sources[0].rateHz == 40e6);
        CHECK(r.meta().sources[0].name == "a.lds");
        CHECK(r.lagTable(0, 0) == nullptr);
        CHECK(r.packedBytes(0) == r.fileSize());
        // From anywhere, across the groups, over the ends, and past them.
        for (const int64_t start : {int64_t(0), int64_t(1), int64_t(3), int64_t(4097), int64_t(whole) - 10, int64_t(-5), int64_t(whole) + 7}) {
            std::vector<int32_t> got(30);
            r.read(0, start, got.size(), got.data());
            bool same = true;
            for (size_t i = 0; i < got.size(); ++i) {
                const int64_t p = start + static_cast<int64_t>(i);
                same = same && got[i] == (p >= 0 && p < static_cast<int64_t>(whole) ? values[static_cast<size_t>(p)] : 0);
            }
            CHECK(same);
        }
    }

    // .s16: the bits found from the samples, or given.
    {
        QByteArray bytes(static_cast<qsizetype>(samples * 2), 0);
        for (size_t i = 0; i < samples; ++i) {
            const int16_t s = static_cast<int16_t>(values[i]);
            std::memcpy(bytes.data() + 2 * i, &s, 2);
        }
        writeFile(dir.filePath("b.s16"), bytes);
        rfx::CaptureSpec spec;
        spec.rateHz = 30e6;
        rfx::TrackReader found(dir.filePath("b.s16"), spec);
        CHECK(found.meta().sources[0].shift == 6);
        CHECK(rfx::carriedBits(rfx::SampleFormat::S16, found.meta().sources[0].shift) == 10);
        spec.bits = 12;
        rfx::TrackReader given(dir.filePath("b.s16"), spec);
        CHECK(given.meta().sources[0].shift == 4);
        CHECK(rfx::shiftForBits(rfx::SampleFormat::S8, 6) == 2);
        CHECK(rfx::shiftForBits(rfx::SampleFormat::S16, 10) == 6);
        std::vector<int32_t> got(20);
        found.read(0, 50, got.size(), got.data());
        CHECK(got[0] == values[50] && got[19] == values[69]);
    }

    // 8-bit and unsigned formats: the samples as the format gives them.
    {
        QByteArray bytes(1000, 0);
        for (int i = 0; i < 1000; ++i) {
            bytes[i] = static_cast<char>(i * 7);
        }
        writeFile(dir.filePath("c.r8"), bytes);
        rfx::CaptureSpec spec;
        spec.format = rfx::SampleFormat::U8;
        spec.rateHz = 20e6;
        rfx::TrackReader r(dir.filePath("c.r8"), spec);
        std::vector<int32_t> got(5);
        r.read(0, 10, got.size(), got.data());
        CHECK(got[0] == static_cast<uint8_t>(10 * 7) && got[4] == static_cast<uint8_t>(14 * 7));
    }

    // Nothing to read, or nothing there: refused.
    writeFile(dir.filePath("empty.s16"), QByteArray());
    writeFile(dir.filePath("short.lds"), QByteArray(3, 1));
    for (const char *name : {"empty.s16", "short.lds", "missing.s16"}) {
        rfx::CaptureSpec spec;
        spec.format = std::string(name).find(".lds") != std::string::npos ? rfx::SampleFormat::Lds : rfx::SampleFormat::S16;
        bool refused = false;
        try {
            rfx::TrackReader r(dir.filePath(name), spec);
        } catch (const rfx::Error &) {
            refused = true;
        }
        CHECK(refused);
    }
}

// When a capture was made comes from the .yaml beside it (the Domesday Duplicator's).
void testCaptureTimes()
{
    const std::string yaml = "\"capture\":\n  \"file\": \"a.ddd.s16\"\n  \"sample_rate_hz\": 75000000\n  \"started\": \"2026-10-27T07:33:55\"\n  \"finished\": \"2026-10-27T07:34:05\"\n  \"duration_seconds\": 10.793\n";
    rfx::Sidecar other;
    other.name = "x.txt";
    other.data = {'n', 'o'};
    rfx::Sidecar sc;
    sc.name = "a.ddd.yaml";
    sc.data.assign(yaml.begin(), yaml.end());
    const std::optional<rfx::CaptureTimes> times = rfx::captureTimes({other, sc});
    CHECK(times.has_value());
    if (times) {
        CHECK(times->started == "2026-10-27T07:33:55");
        CHECK(times->finished == "2026-10-27T07:34:05");
        CHECK(std::fabs(times->durationSeconds - 10.793) < 1e-9);
    }
    CHECK(!rfx::captureTimes({other}).has_value());
    CHECK(!rfx::captureTimes({}).has_value());
}

// The .yaml of the Domesday Duplicator, laid out in sections, and what it declares of the samples.
void testSidecarSections()
{
    const std::string yaml =
        "# a comment\n\n\"schema_version\": 1\n\"application_version\": \"10fc5c35\"\n\n\"capture\":\n  \"file\": \"a.ddd.s16\"\n  \"format\": \"signed 16-bit\"\n"
        "  \"sample_rate_hz\": 75000000\n  \"bit_shift\": 0\n  \"completed\": true\n\n\"signal\":\n  \"rms\": 85.49\n\n\"naming\": {}\n\n\"disc\": {}\n";
    rfx::Sidecar sc;
    sc.name = "a.ddd.yaml";
    sc.data.assign(yaml.begin(), yaml.end());
    const std::vector<rfx::SidecarSection> sections = rfx::sidecarSections({sc});
    CHECK(sections.size() == 5);
    if (sections.size() == 5) {
        CHECK(sections[0].name == "file" && sections[0].rows.size() == 2 && sections[0].rows[1] == std::make_pair(std::string("application_version"), std::string("10fc5c35")));
        CHECK(sections[1].name == "capture" && sections[1].rows.size() == 5);
        CHECK(sections[1].rows[1] == std::make_pair(std::string("format"), std::string("signed 16-bit")));
        CHECK(sections[2].name == "signal" && sections[2].rows.size() == 1 && sections[2].rows[0].second == "85.49");
        CHECK(sections[3].name == "naming" && sections[3].rows.empty());
    }
    const std::optional<rfx::DeclaredCapture> declared = rfx::declaredCapture({sc});
    CHECK(declared.has_value());
    if (declared) {
        CHECK(declared->format == rfx::SampleFormat::S16);
        CHECK(!declared->compressed);
        CHECK(declared->rateHz == 75e6);
    }
    // A FLAC capture says so; a file with no format, or no rate, declares nothing; a file that is not that yaml has no sections.
    std::string flac = yaml;
    flac.replace(flac.find("signed 16-bit"), 13, "FLAC");
    rfx::Sidecar f;
    f.data.assign(flac.begin(), flac.end());
    CHECK(rfx::declaredCapture({f}) && rfx::declaredCapture({f})->compressed);
    std::string norate = yaml;
    norate.replace(norate.find("\"sample_rate_hz\""), 16, "\"sample_rate_x\"");
    rfx::Sidecar n;
    n.data.assign(norate.begin(), norate.end());
    CHECK(!rfx::declaredCapture({n}).has_value());
    rfx::Sidecar other;
    other.data = {'n', 'o'};
    CHECK(rfx::sidecarSections({other}).empty());
    CHECK(!rfx::declaredCapture({}).has_value());
}

// The header of a compressed capture says its rate (in kHz, as ld-decode writes it) and its sample size.
void testCompressedHeader()
{
    QTemporaryDir dir;
    const QString flac = dir.filePath("x.flac");
    QProcess make;
    make.start(rfx::ffmpegProgram(), {"-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=40000:duration=1", "-sample_fmt", "s16", "-c:a", "flac", flac});
    if (!make.waitForStarted() || !make.waitForFinished(30000) || make.exitCode() != 0) {
        std::puts("rf-analyse-tests: no ffmpeg, the header of a compressed capture is not tested");
        return;
    }
    const std::optional<rfx::CompressedHeader> header = rfx::probeCompressed(flac);
    CHECK(header.has_value());
    if (header) {
        CHECK(header->headerRate == 40000);
        CHECK(header->bits == 16);
    }
    CHECK(!rfx::probeCompressed(dir.filePath("missing.flac")).has_value());
}

// A compressed capture is read where it is asked for: ffmpeg jumps there, and the samples are exact.
void testCompressedReader()
{
    QTemporaryDir dir;
    // A ramp that wraps, so that a sample that is out of place is seen.
    constexpr int64_t kSamples = 1500000;
    const auto expected = [](int64_t i) { return static_cast<int32_t>(i % 30011 - 15000); };
    {
        QByteArray raw(static_cast<qsizetype>(kSamples * 2), 0);
        for (int64_t i = 0; i < kSamples; ++i) {
            const int16_t v = static_cast<int16_t>(expected(i));
            std::memcpy(raw.data() + 2 * i, &v, 2);
        }
        writeFile(dir.filePath("ramp.raw"), raw);
    }
    QProcess make;
    make.start(rfx::ffmpegProgram(), {"-v", "error", "-y", "-f", "s16le", "-ar", "75000", "-ac", "1", "-i", dir.filePath("ramp.raw"), "-c:a", "flac", dir.filePath("ramp.flac")});
    if (!make.waitForStarted() || !make.waitForFinished(60000) || make.exitCode() != 0) {
        std::puts("rf-analyse-tests: no ffmpeg, the compressed reader is not tested");
        return;
    }
    const std::optional<rfx::FlacInfo> info = rfx::readFlacInfo(dir.filePath("ramp.flac"));
    CHECK(info.has_value() && info->sampleRate == 75000 && info->channels == 1 && info->bits == 16 && info->samples == static_cast<uint64_t>(kSamples));

    rfx::CaptureSpec spec;
    spec.rateHz = 75e6;
    spec.compressed = true;
    rfx::TrackReader reader(dir.filePath("ramp.flac"), spec);
    CHECK(reader.isCompressed() && !reader.isArchive());
    CHECK(reader.meta().sources[0].words == static_cast<uint64_t>(kSamples));
    CHECK(reader.meta().sources[0].rateHz == 75e6);

    const auto matches = [&](int64_t start, size_t n, bool once) {
        std::vector<int32_t> got(n, 12345);
        once ? reader.readOnce(0, start, n, got.data()) : reader.read(0, start, n, got.data());
        for (size_t i = 0; i < n; ++i) {
            const int64_t at = start + static_cast<int64_t>(i);
            const int32_t want = at >= 0 && at < kSamples ? expected(at) : 0;
            if (got[i] != want) {
                std::fprintf(stderr, "compressed read from %lld: sample %zu is %d, not %d\n", static_cast<long long>(start), i, got[i], want);
                return false;
            }
        }
        return true;
    };
    // The header can lie (a capture written to a pipe): the last frames know how long the file is.
    {
        CHECK(QFile::copy(dir.filePath("ramp.flac"), dir.filePath("lying.flac")));
        QFile lying(dir.filePath("lying.flac"));
        CHECK(lying.open(QIODevice::ReadWrite));
        // The 36 bits of the number of samples: the low 4 bits of byte 21 (the bits per sample are in its high 4) and bytes 22
        // to 25. 0x28000000 samples.
        lying.seek(21);
        char keep = 0;
        lying.getChar(&keep);
        lying.seek(21);
        lying.putChar(static_cast<char>(keep & 0xF0));
        const char bogus[4] = {0x28, 0x00, 0x00, 0x00};
        lying.write(bogus, 4);
        lying.close();
        const std::optional<rfx::FlacInfo> lied = rfx::readFlacInfo(dir.filePath("lying.flac"));
        CHECK(lied.has_value() && lied->samples == 0x28000000ULL);
        const std::optional<uint64_t> found = lied ? rfx::flacSamplesFromTail(dir.filePath("lying.flac"), *lied) : std::nullopt;
        CHECK(found.has_value() && *found == static_cast<uint64_t>(kSamples));
        rfx::TrackReader liar(dir.filePath("lying.flac"), spec);
        CHECK(liar.meta().sources[0].words == static_cast<uint64_t>(kSamples));
    }
    // Where it is asked for: inside a stretch, over the border of two, over several, at the ends, past them.
    CHECK(matches(0, 1000, false));
    CHECK(matches(262140, 20, false));
    CHECK(matches(700001, 600000, false));
    CHECK(matches(kSamples - 500, 500, false));
    CHECK(matches(kSamples - 100, 300, false));
    CHECK(matches(-50, 100, false));
    CHECK(matches(kSamples + 10, 50, false));
    // Again, from what is held, and without keeping anything.
    CHECK(matches(700001, 600000, false));
    CHECK(matches(900123, 40000, true));
    CHECK(matches(1234567, 4096, true));
}

} // namespace

// The RF of an NTSC disc, made up: the carrier is 8.1 MHz at blanking and 7.614 MHz in a sync pulse; the lines are 63.556 us apart,
// the vertical interval has its equalizing and broad pulses at every half line, and the fields come as the standard says (a full
// line, then a half line, between the last horizontal sync and the interval).
void testRfSync()
{
    const double fs = 40e6;
    const double hlineUs = 227.5e6 / (315.0e6 / 88.0) / 2.0;
    const double hz = 1.7e6 / 140.0;
    const double blank = 8.1e6;
    const double sync = blank - 40.0 * hz;
    const size_t halfLines = 1050 * 3;
    const size_t samples = static_cast<size_t>(static_cast<double>(halfLines) * hlineUs * fs / 1e6) + 4096;
    std::vector<float> frequency(samples, static_cast<float>(blank));
    auto pulse = [&](double startUs, double lengthUs) {
        const size_t a = static_cast<size_t>(startUs * fs / 1e6);
        const size_t b = std::min(samples, static_cast<size_t>((startUs + lengthUs) * fs / 1e6));
        for (size_t i = a; i < b; ++i) {
            frequency[i] = static_cast<float>(sync);
        }
    };
    for (size_t h = 0; h < halfLines; ++h) {
        const size_t inFrame = h % 1050;
        const double at = static_cast<double>(h) * hlineUs;
        const size_t vbiStart = inFrame < 525 ? 0 : 525;
        if (inFrame >= vbiStart && inFrame < vbiStart + 18) {
            const size_t k = inFrame - vbiStart;
            pulse(at, (k >= 6 && k < 12) ? 27.1 : 2.3);
        } else if (inFrame % 2 == 0) {
            pulse(at, 4.7);
        }
    }
    // Frequency to samples, phase by phase (a little noise keeps the filters honest).
    std::vector<int16_t> pcm(samples);
    Lcg noise{7};
    double phase = 0.0;
    for (size_t i = 0; i < samples; ++i) {
        phase += 2.0 * 3.14159265358979323846 * static_cast<double>(frequency[i]) / fs;
        phase = std::fmod(phase, 2.0 * 3.14159265358979323846);
        pcm[i] = static_cast<int16_t>(std::lround(9000.0 * std::sin(phase) + (uniform(noise) - 0.5) * 60.0));
    }
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath("ntsc.ddd.s16");
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(reinterpret_cast<const char *>(pcm.data()), static_cast<qint64>(pcm.size() * 2));
    }
    rfx::CaptureSpec spec;
    spec.format = rfx::SampleFormat::S16;
    spec.rateHz = fs;
    spec.bits = 16;
    rfx::TrackReader reader(path, spec);
    const std::vector<rfx::Field> fields = rfx::locateFields(reader, 0, fs, rfx::VideoSystem::Ntsc, 2);
    // Six fields were made; the first and the last are not complete.
    CHECK(fields.size() >= 4);
    for (size_t i = 0; i < fields.size(); ++i) {
        CHECK(fields[i].first == (i % 2 == 0) || fields[i].first == (i % 2 == 1));
        if (i > 0) {
            CHECK(fields[i].first != fields[i - 1].first);
        }
        CHECK(fields[i].lines.size() > 250);
        // The lines are 63.556 us apart: 2542.2 samples at 40 MHz.
        for (size_t l = 1; l < 200 && l < fields[i].lines.size(); ++l) {
            CHECK(std::abs(fields[i].lines[l] - fields[i].lines[l - 1] - 2542.22) < 1.5);
        }
    }
    // The dump is the one ld-decode writes: "true None" and the positions as hexadecimal bits.
    const std::string dump = rfx::formatPass0(fields);
    CHECK(dump.rfind("true None ", 0) == 0 || dump.rfind("false None ", 0) == 0);
    // A capture with no video in it gives no field, and no error.
    std::vector<int16_t> silence(1 << 20, 0);
    const QString quiet = dir.filePath("quiet.ddd.s16");
    {
        QFile f(quiet);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(reinterpret_cast<const char *>(silence.data()), static_cast<qint64>(silence.size() * 2));
    }
    rfx::TrackReader emptyReader(quiet, spec);
    CHECK(rfx::locateFields(emptyReader, 0, fs, rfx::VideoSystem::Ntsc, 2).empty());
}

// The FLAC and the raw variant of an .rfx file are read where they are asked for, as the compressed capture is: the tracks come out
// as the canonical samples of the format they were packed in.
void testStoredReader()
{
    QTemporaryDir dir;
    constexpr int64_t kSamples = 900000;
    const auto expected = [](int64_t i) { return static_cast<int32_t>(i % 30011 - 15000); };
    QByteArray s16(static_cast<qsizetype>(kSamples * 2), 0);
    QByteArray r16(static_cast<qsizetype>(kSamples * 2), 0);
    for (int64_t i = 0; i < kSamples; ++i) {
        const int16_t v = static_cast<int16_t>(expected(i));
        const uint16_t u = static_cast<uint16_t>(expected(i) + 32768);
        std::memcpy(s16.data() + 2 * i, &v, 2);
        std::memcpy(r16.data() + 2 * i, &u, 2);
    }
    writeFile(dir.filePath("a.ddd.s16"), s16);
    writeFile(dir.filePath("b.ddd.r16"), r16);
    rfx::PackOptions options;
    options.rateMhz = 40.0;
    options.threads = 2;
    for (const rfx::StoredCodec codec : {rfx::StoredCodec::Raw, rfx::StoredCodec::Flac}) {
        const QString file = dir.filePath(codec == rfx::StoredCodec::Raw ? "raw.rfx" : "flac.rfx");
        try {
            rfx::packFlac({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.r16")}, file, options, codec);
        } catch (const rfx::Error &) {
            std::puts("rf-analyse-tests: no ffmpeg, the reader of the FLAC and raw variants is not tested");
            return;
        }
        rfx::TrackReader reader(file);
        CHECK(reader.isArchive() && !reader.isCompressed());
        CHECK(reader.trackCount() == 2);
        CHECK(reader.meta().sources[0].words == static_cast<uint64_t>(kSamples));
        CHECK(reader.meta().sources[1].fmt == "r16");
        CHECK(reader.meta().sources[0].rateHz == 40e6);
        CHECK(reader.lagTable(1, 0) == nullptr);
        for (size_t k = 0; k < 2; ++k) {
            const int32_t offset = k == 1 ? 32768 : 0;
            for (const bool once : {false, true}) {
                for (const int64_t start : {int64_t(-100), int64_t(0), int64_t(262140), int64_t(500000), kSamples - 50}) {
                    const size_t n = 400;
                    std::vector<int32_t> got(n, 12345);
                    once ? reader.readOnce(k, start, n, got.data()) : reader.read(k, start, n, got.data());
                    for (size_t i = 0; i < n; ++i) {
                        const int64_t at = start + static_cast<int64_t>(i);
                        const int32_t want = at >= 0 && at < kSamples ? expected(at) + offset : 0;
                        if (got[i] != want) {
                            std::fprintf(stderr, "stored read of track %zu from %lld: sample %zu is %d, not %d\n", k, static_cast<long long>(start), i, got[i], want);
                            ++failures;
                            break;
                        }
                    }
                }
            }
        }
    }
}

int main()


{
    testSpectrum();
    testReader();
    testCaptureReader();
    testSpeed();
    testCaptureTimes();
    testSidecarSections();
    testCompressedHeader();
    testCompressedReader();
    testStoredReader();
    testRfSync();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("rf-analyse-tests: ok");
    return 0;
}
