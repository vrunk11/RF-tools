/******************************************************************************
 * requantize_tests.cpp
 * rfx library - the requantiser: that it decides what the capture application's
 * requantiser decides (fixed samples out for fixed samples in), and that it
 * reads and writes captures and .rfx files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "archive.h"
#include "error.h"
#include "ffmpeg.h"
#include "flacmode.h"
#include "packer.h"
#include "requantize.h"
#include "requantizer.h"
#include "sha256.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
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

constexpr double kRateMhz = 30.0;

// The hashes of what the requantiser of the Domesday Duplicator's capture
// application (ddd-gui, rf_requantizer.cpp) makes of syntheticCapture(), run with
// the settings of each case below. They come from that program, not from this
// one: a change here that alters them alters what is decided.
const char *const kInputHash = "f3d8cc33dfeeb702aaf0d11d3f3d6798f1df4f6da85b183305a82447b848273f";
// margin 0 (aggressive), fixed shaping, the default bands: one bit dropped.
const char *const kAggressiveHash = "cc7f809fc4e234a8ff6dab2f62ca29f3eabafc24aeee2b0ada889ec21196cafc";
// margin 0, adaptive shaping, bands 0-6, depth 30: four bits dropped, shaped.
const char *const kAdaptiveHash = "2206a65e9a7c6b0473d38db6665c3cf5f6a71651ee9122a170911c5089bffeb1";

struct Lcg
{
    uint32_t x;
    uint32_t next()
    {
        x = x * 1664525u + 1013904223u;
        return x;
    }
};

// Tones and noise on the 10-bit scale of the Duplicator, left-aligned in 16 bits:
// a capture with a noise floor of a few LSB, which a requantiser can cut.
QByteArray syntheticCapture(size_t samples, uint32_t seed)
{
    Lcg lcg{seed};
    QByteArray out(static_cast<qsizetype>(samples * 2), 0);
    for (size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i);
        double noise = 0;
        for (int k = 0; k < 4; ++k) {
            noise += static_cast<double>(lcg.next() >> 16) / 65536.0 - 0.5;
        }
        const double v = 180 * std::sin(t * 0.21) + 120 * std::sin(t * 0.0137 + 1) + 60 * std::sin(t * 0.77) + noise * 4.5;
        int ten = static_cast<int>(std::lround(v));
        ten = ten > 511 ? 511 : (ten < -512 ? -512 : ten);
        const int16_t s = static_cast<int16_t>(ten * 64);
        out[static_cast<qsizetype>(2 * i)] = static_cast<char>(s & 0xFF);
        out[static_cast<qsizetype>(2 * i + 1)] = static_cast<char>((s >> 8) & 0xFF);
    }
    return out;
}

std::string sha(const QByteArray &data)
{
    rfx::Sha256 hash;
    hash.update(data.constData(), static_cast<size_t>(data.size()));
    return hash.finishHex();
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray("<missing>");
    }
    return f.readAll();
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        std::fprintf(stderr, "cannot write %s\n", path.toStdString().c_str());
        ++failures;
    }
}

// The requantiser's options for the two settings that are pinned.
rfx::RequantizeOptions aggressive()
{
    rfx::RequantizeOptions o;
    o.marginLevel = 0;
    return o;
}

rfx::RequantizeOptions adaptive()
{
    rfx::RequantizeOptions o;
    o.marginLevel = 0;
    o.adaptive = true;
    o.bands = rfx::parseBands("0-6");
    o.depthsDb = rfx::parseShapingDepths("30");
    return o;
}

// `data` (signed 16-bit samples) through the requantiser, in memory.
QByteArray requantizeBytes(const QByteArray &data, const rfx::RequantizeOptions &options, rfx::RequantizeResult *result = nullptr)
{
    const size_t total = static_cast<size_t>(data.size()) / 2;
    QByteArray out(data.size(), 0);
    size_t readAt = 0;
    size_t writeAt = 0;
    rfx::StreamSpec spec;
    spec.name = "synthetic";
    spec.rateMhz = kRateMhz;
    spec.total = total;
    const rfx::RequantizeResult r = rfx::requantizeStream(
        options, spec,
        [&](int16_t *dst, size_t count) {
            const size_t n = std::min(count, total - readAt);
            std::memcpy(dst, data.constData() + 2 * readAt, 2 * n);
            readAt += n;
            return n;
        },
        [&](const int16_t *samples, size_t count) {
            std::memcpy(out.data() + 2 * writeAt, samples, 2 * count);
            writeAt += count;
        });
    if (result) {
        *result = r;
    }
    return out;
}

void testParsing()
{
    using rfx::FrequencyBand;
    CHECK(rfx::parseBands("0-12") == (std::vector<FrequencyBand>{{0.0, 12.0}}));
    CHECK(rfx::parseBands(" 0 - 1.9 , 2.1:13.5 MHz") == (std::vector<FrequencyBand>{{0.0, 1.9}, {2.1, 13.5}}));
    CHECK(rfx::parseBands("12-2").empty());
    CHECK(rfx::parseBands("0-12,").empty());
    CHECK(rfx::parseBands("a-b").empty());
    CHECK(rfx::parseBands("").empty());
    CHECK(rfx::describeBands({{0.0, 13.5}, {20.0, 25.0}}) == "0-13.5, 20-25 MHz");

    CHECK(rfx::parseShapingDepths("40") == std::vector<double>{40.0});
    CHECK(rfx::parseShapingDepths("10, 40 dB") == (std::vector<double>{10.0, 40.0}));
    CHECK(rfx::parseShapingDepths("41").empty());
    CHECK(rfx::parseShapingDepths("10,").empty());
    CHECK(rfx::describeShapingDepths({10.0, 40.0}) == "10, 40 dB");

    // With bands 2-14 at 35 Msps, "10, 40" puts 10 dB below 2 MHz and 40 dB above 14 MHz.
    const std::vector<rfx::ShapingZone> zones = rfx::gapShapingZones({{2.0, 14.0}}, {10.0, 40.0}, 35.0);
    CHECK(zones == (std::vector<rfx::ShapingZone>{{0.0, 2.0, 10.0}, {14.0, 17.5, 40.0}}));

    // At 30 Msps the default band leaves one stretch: two depths are too many.
    rfx::RequantizerRequest request;
    request.sampleRateMhz = kRateMhz;
    request.depthsDb = {10.0, 40.0};
    CHECK(!rfx::shapingDepthsProblem(request).empty());
    request.depthsDb = {10.0};
    CHECK(rfx::shapingDepthsProblem(request).empty());

    const rfx::RequantizerSettings settings = rfx::settingsFor(request);
    CHECK(settings.protectedBands == (std::vector<rfx::FrequencyBand>{{0.0, 13.5}}));
    CHECK(settings.shapingDepthDb == 10.0);
    CHECK(settings.shapingOrder == 16);
}

void testQuantizer()
{
    // Ten of the sixteen bits kept: a step of 64, ties to even, saturating one step
    // under full scale.
    rfx::ShapingQuantizer plain(10, {1.0});
    CHECK(plain.step() == 64.0);
    CHECK(plain.quantize(100) == 128);
    CHECK(plain.quantize(96) == 128);
    CHECK(plain.quantize(32) == 0);
    CHECK(plain.quantize(-32) == 0);
    CHECK(plain.quantize(-100) == -128);
    CHECK(plain.clipped() == 0);
    CHECK(plain.quantize(32767) == 32704);
    CHECK(plain.quantize(-32768) == -32768);
    CHECK(plain.clipped() == 1);
}

void testGoldenSamples()
{
    const QByteArray input = syntheticCapture(1100000, 12345);
    CHECK(sha(input) == kInputHash);

    rfx::RequantizeResult result;
    const QByteArray cut = requantizeBytes(input, aggressive(), &result);
    CHECK(sha(cut) == kAggressiveHash);
    CHECK(result.inputBits == 10);
    CHECK(result.summary.samples == 1100000);
    CHECK(result.summary.segments == 2);
    CHECK(result.summary.samplesByDrop.size() == 2 && result.summary.samplesByDrop[1] == 1100000);
    CHECK(result.summary.shapedSamples == 0);
    CHECK(result.summary.changes.size() == 1);

    const QByteArray shaped = requantizeBytes(input, adaptive(), &result);
    CHECK(sha(shaped) == kAdaptiveHash);
    CHECK(result.summary.samplesByDrop.size() == 5 && result.summary.samplesByDrop[4] == 1100000);
    CHECK(result.summary.shapedSamples == 1100000);

    // Every sample is a whole multiple of the step of the bits kept.
    bool multiples = true;
    for (qsizetype i = 0; i + 1 < shaped.size(); i += 2) {
        int16_t s = 0;
        std::memcpy(&s, shaped.constData() + i, 2);
        multiples = multiples && (s % (64 * 16) == 0);
    }
    CHECK(multiples);

    // Margin off is the reference: the samples as they are.
    rfx::RequantizeOptions off;
    off.requantize = false;
    CHECK(requantizeBytes(input, off) == input);
}

// The samples of a file as the stream above makes them, to compare a job with.
void testJobsOnAnArchive()
{
    QTemporaryDir dir;
    const QByteArray input = syntheticCapture(1100000, 12345);
    const QByteArray expected = requantizeBytes(input, adaptive());
    writeFile(dir.filePath("syn.s16"), input);

    rfx::PackOptions pack;
    pack.rateMhz = kRateMhz;
    pack.threads = 2;
    pack.order = rfx::OrderMode::Given;
    pack.segment = 65536;
    rfx::packCaptures({dir.filePath("syn.s16")}, dir.filePath("syn.rfx"), pack);

    rfx::RequantizeJob job;
    job.options = adaptive();
    job.pack = pack;

    // .rfx in, .rfx out: the track comes back requantised, and says its rate.
    job.input = dir.filePath("syn.rfx");
    job.output = dir.filePath("cut.rfx");
    const std::vector<rfx::RequantizeResult> results = rfx::runRequantize(job);
    CHECK(results.size() == 1 && results[0].inputBits == 10 && results[0].rateMhz == kRateMhz);
    rfx::unpackArchive(dir.filePath("cut.rfx"), dir.filePath("un"), rfx::UnpackOptions());
    CHECK(sha(readAll(dir.filePath("un/syn.s16"))) == kAdaptiveHash);
    CHECK(readAll(dir.filePath("un/syn.s16")) == expected);
    CHECK(QFileInfo(dir.filePath("cut.rfx")).size() < QFileInfo(dir.filePath("syn.rfx")).size());
    // No work directory is left behind.
    CHECK(QDir(dir.path()).entryList(QStringList{"rf-requantize-*"}, QDir::Dirs).isEmpty());

    // A capture in, the same capture out; and the report alone.
    job.input = dir.filePath("syn.s16");
    job.rateMhz = kRateMhz;
    job.output = dir.filePath("cut.s16");
    rfx::runRequantize(job);
    CHECK(readAll(dir.filePath("cut.s16")) == expected);

    job.output.clear();
    const std::vector<rfx::RequantizeResult> preview = rfx::runRequantize(job);
    CHECK(preview.size() == 1 && preview[0].summary.shapedSamples == 1100000);

    // The tool does not change formats, and a command line that asks for it, a
    // capture whose rate nobody knows and a job that would overwrite its input are
    // the command line's fault.
    const auto refuses = [](const rfx::RequantizeJob &j) {
        try {
            rfx::runRequantize(j);
        } catch (const rfx::UsageError &) {
            return true;
        }
        return false;
    };
    rfx::RequantizeJob wrong = job;
    wrong.input = dir.filePath("syn.rfx");
    wrong.output = dir.filePath("wrong.s16");
    CHECK(refuses(wrong)); // .rfx to a capture
    wrong.input = dir.filePath("syn.s16");
    wrong.output = dir.filePath("wrong.rfx");
    CHECK(refuses(wrong)); // a capture to .rfx
    wrong.output = dir.filePath("wrong.r16");
    CHECK(refuses(wrong)); // a capture to another format
    wrong.output = dir.filePath("wrong.flac");
    CHECK(refuses(wrong)); // a raw capture to FLAC
    wrong = job;
    wrong.rateMhz.reset();
    CHECK(refuses(wrong));
    wrong = job;
    wrong.output = job.input;
    CHECK(refuses(wrong));
    CHECK(readAll(dir.filePath("syn.s16")) == input);
    CHECK(!QFile::exists(dir.filePath("wrong.s16")) && !QFile::exists(dir.filePath("wrong.rfx")));
}

// Two captures of one disc, each requantised on its own, with the .yaml beside the
// first kept and the lag between them still the archive's.
void testJobsOnTwoTracks()
{
    QTemporaryDir dir;
    const size_t samples = 3000000;
    const QByteArray a = syntheticCapture(samples, 1);
    const QByteArray b = syntheticCapture(samples, 2);
    writeFile(dir.filePath("a.ddd.s16"), a);
    writeFile(dir.filePath("b.ddd.s16"), b);
    writeFile(dir.filePath("a.ddd.yaml"), "{\n  \"sample_rate_hz\": 30000000\n}\n");
    writeFile(dir.filePath("b.ddd.yaml"), "{\n  \"sample_rate_hz\": 30000000\n}\n");

    // The dump of ld-decode's pass 0 for 75 frames of two fields of 20 lines
    // 1000 samples apart, which cover the three million samples.
    const auto dump = [](double first) {
        std::string text;
        const double fieldLength = 20 * 1000.0;
        for (size_t f = 0; f < 75; ++f) {
            for (int half = 0; half < 2; ++half) {
                text += half == 0 ? "true " : "false ";
                text += "Some([" + std::to_string(f + 1000) + ", 0])";
                const double start = first + (static_cast<double>(f) * 2 + half) * fieldLength;
                for (size_t i = 0; i < 20; ++i) {
                    const double pos = start + static_cast<double>(i) * 1000.0;
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
    };
    QDir().mkpath(dir.filePath("p0"));
    writeFile(dir.filePath("p0/pass0_0.txt"), dump(500));
    writeFile(dir.filePath("p0/pass0_1.txt"), dump(500));

    rfx::PackOptions pack;
    pack.threads = 2;
    pack.order = rfx::OrderMode::Given;
    pack.segment = 65536;
    pack.pass0Dir = dir.filePath("p0");
    rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, dir.filePath("two.rfx"), pack);

    // Requantising needs no pass 0: the lines are where they were, and the archive's
    // lag tables are kept. A program that does not exist stands for ld-decode.
    rfx::RequantizeJob job;
    job.options = aggressive();
    job.pack = pack;
    job.pack.pass0Dir.reset();
    job.pack.ldDecode = QStringLiteral("no-such-ld-decode");
    job.input = dir.filePath("two.rfx");
    job.output = dir.filePath("cut.rfx");
    const std::vector<rfx::RequantizeResult> results = rfx::runRequantize(job);
    CHECK(results.size() == 2);

    rfx::unpackArchive(dir.filePath("cut.rfx"), dir.filePath("un"), rfx::UnpackOptions());
    CHECK(readAll(dir.filePath("un/a.ddd.s16")) == requantizeBytes(a, aggressive()));
    CHECK(readAll(dir.filePath("un/b.ddd.s16")) == requantizeBytes(b, aggressive()));
    CHECK(readAll(dir.filePath("un/a.ddd.yaml")) == readAll(dir.filePath("a.ddd.yaml")));
    CHECK(readAll(dir.filePath("un/b.ddd.yaml")) == readAll(dir.filePath("b.ddd.yaml")));
    {
        rfx::Archive opened(dir.filePath("cut.rfx"));
        CHECK(opened.meta().sources.size() == 2);
        CHECK(opened.meta().sources[0].rateHz == 30000000.0);
        CHECK(opened.meta().sources[0].name == "a.ddd.s16" && opened.meta().sources[1].name == "b.ddd.s16");

        // The second track is still predicted from the first, the way it was, with
        // the same lag table.
        rfx::Archive before(dir.filePath("two.rfx"));
        CHECK(opened.meta().sources[1].refs == before.meta().sources[1].refs);
        CHECK(opened.meta().sources[1].weights == before.meta().sources[1].weights);
        CHECK(opened.meta().sources[1].own == before.meta().sources[1].own);
        const auto oldTable = before.tables().find({1, 0});
        const auto newTable = opened.tables().find({1, 0});
        CHECK(oldTable != before.tables().end() && newTable != opened.tables().end());
        if (oldTable != before.tables().end() && newTable != opened.tables().end()) {
            CHECK(oldTable->second.pos == newTable->second.pos && oldTable->second.lag == newTable->second.lag);
        }
        // And it still costs less than the first: the reference works.
        CHECK(opened.packedBytes(1) < opened.packedBytes(0));
    }

    // One track of two: an .rfx file of one, which no longer has the reference it
    // was predicted from.
    job.only = {1};
    job.output = dir.filePath("only.rfx");
    const std::vector<rfx::RequantizeResult> one = rfx::runRequantize(job);
    CHECK(one.size() == 1 && one[0].name == "b.ddd.s16");
    rfx::unpackArchive(dir.filePath("only.rfx"), dir.filePath("un_only"), rfx::UnpackOptions());
    CHECK(readAll(dir.filePath("un_only/b.ddd.s16")) == requantizeBytes(b, aggressive()));
    {
        rfx::Archive opened(dir.filePath("only.rfx"));
        CHECK(opened.meta().sources.size() == 1 && opened.meta().sources[0].refs.empty());
        CHECK(opened.meta().sources[0].rateHz == 30000000.0);
    }

    // The rate asked for is the rate written.
    job.only.clear();
    job.rateMhz = 40.0;
    job.output = dir.filePath("rate.rfx");
    rfx::runRequantize(job);
    CHECK(rfx::Archive(dir.filePath("rate.rfx")).meta().sources[1].rateHz == 40000000.0);
}

// FLAC is coded by ffmpeg: where there is none, this is skipped. A .flac comes out
// as a .flac and the FLAC variant of .rfx as the FLAC variant, with the samples the
// requantiser makes of them; a .flac does not come out as an .ldf or a raw capture.
void testFlac()
{
    QProcess probe;
    probe.start(rfx::ffmpegProgram(), {"-version"});
    if (!probe.waitForStarted() || !probe.waitForFinished() || probe.exitCode() != 0) {
        std::fprintf(stderr, "no FLAC test: ffmpeg is not there\n");
        return;
    }
    QTemporaryDir dir;
    const QByteArray input = syntheticCapture(1100000, 12345);
    const QByteArray expected = requantizeBytes(input, adaptive());
    writeFile(dir.filePath("syn.s16"), input);
    // The container rate is the RF rate in kHz: 30 MHz is 30000 Hz.
    rfx::runFfmpeg({"-v", "error", "-y", "-f", "s16le", "-ar", "30000", "-ac", "1", "-i", dir.filePath("syn.s16"), "-c:a", "flac", dir.filePath("syn.flac")},
                   "ffmpeg could not encode the test capture");

    rfx::RequantizeJob job;
    job.options = adaptive();
    job.rateMhz = kRateMhz;
    job.pack.threads = 2;
    job.input = dir.filePath("syn.flac");
    job.output = dir.filePath("cut.flac");
    rfx::runRequantize(job);
    rfx::runFfmpeg({"-v", "error", "-y", "-i", dir.filePath("cut.flac"), "-f", "s16le", "-acodec", "pcm_s16le", dir.filePath("cut.s16")}, "ffmpeg could not decode");
    CHECK(readAll(dir.filePath("cut.s16")) == expected);
    CHECK(QFileInfo(dir.filePath("cut.flac")).size() < QFileInfo(dir.filePath("syn.flac")).size());

    rfx::RequantizeJob wrong = job;
    wrong.output = dir.filePath("wrong.ldf");
    bool refused = false;
    try {
        rfx::runRequantize(wrong);
    } catch (const rfx::UsageError &) {
        refused = true;
    }
    CHECK(refused);
    wrong.output = dir.filePath("wrong.s16");
    refused = false;
    try {
        rfx::runRequantize(wrong);
    } catch (const rfx::UsageError &) {
        refused = true;
    }
    CHECK(refused);

    // The FLAC variant of .rfx: the tracks are requantised and coded as FLAC again.
    rfx::PackOptions pack;
    pack.rateMhz = kRateMhz;
    pack.order = rfx::OrderMode::Given;
    rfx::packFlac({dir.filePath("syn.s16")}, dir.filePath("syn_flac.rfx"), pack);
    CHECK(rfx::isFlacArchive(dir.filePath("syn_flac.rfx")));
    job.input = dir.filePath("syn_flac.rfx");
    job.output = dir.filePath("cut_flac.rfx");
    job.rateMhz.reset();
    rfx::runRequantize(job);
    CHECK(rfx::isFlacArchive(dir.filePath("cut_flac.rfx")));
    rfx::unpackFlac(dir.filePath("cut_flac.rfx"), dir.filePath("un"), std::nullopt, std::nullopt);
    CHECK(readAll(dir.filePath("un/syn.s16")) == expected);
}

} // namespace

int main()
{
    testParsing();
    testQuantizer();
    testGoldenSamples();
    testJobsOnAnArchive();
    testJobsOnTwoTracks();
    testFlac();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("rf-requantize-tests: ok");
    return 0;
}
