/******************************************************************************
 * roundtrip_tests.cpp
 * rfx library - pack, edit and unpack synthetic captures, and check that what
 * comes out is what went in (a lossless tool is tested by the round trip)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "archive.h"
#include "edit.h"
#include "error.h"
#include "opencl.h"
#include "packer.h"
#include "sampleformat.h"

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

// What the options of a quiet test are: small segments, two threads.
rfx::PackOptions testOptions()
{
    rfx::PackOptions o;
    o.segment = 65536;
    o.threads = 2;
    o.order = rfx::OrderMode::Given;
    return o;
}

// A signal made of a few tones and some noise, as 16-bit samples shifted left by 6
// bits (the Domesday Duplicator's 10 bits), starting `lag` samples into the signal.
QByteArray duplicatorCapture(size_t samples, int lag, uint32_t noiseSeed)
{
    QByteArray out(static_cast<qsizetype>(samples * 2), 0);
    Lcg noise{noiseSeed};
    for (size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(static_cast<int64_t>(i) + lag);
        const double v = 180 * std::sin(t * 0.21) + 120 * std::sin(t * 0.0137 + 1) + 60 * std::sin(t * 0.77);
        const int tenBit = static_cast<int>(std::lround(v)) + static_cast<int>(noise.next() >> 29) - 3;
        const int16_t s = static_cast<int16_t>(tenBit * 64);
        out[static_cast<qsizetype>(2 * i)] = static_cast<char>(s & 0xFF);
        out[static_cast<qsizetype>(2 * i + 1)] = static_cast<char>((s >> 8) & 0xFF);
    }
    return out;
}

// The dump of ld-decode's pass 0 for a capture whose frames are `frames`: every
// frame has two fields of `lines` lines, `spacing` samples apart, the first one at
// `first` + the frame's place.
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

// One capture in a raw format of every kind, with a tail, packed alone and unpacked.
void testEveryFormat()
{
    QTemporaryDir dir;
    struct Case
    {
        const char *name;
        size_t bytes;
        bool lds;
    };
    const Case cases[] = {
        {"a.s16", 400001, false}, {"b.r16", 400000, false}, {"c.s8", 300003, false}, {"d.r8", 300000, false}, {"e.lds", 250003, true},
    };
    for (const Case &c : cases) {
        Lcg lcg{12345};
        QByteArray data(static_cast<qsizetype>(c.bytes), 0);
        for (qsizetype i = 0; i < data.size(); ++i) {
            // A slow wave and a little noise, so that there is something to predict.
            const int wave = static_cast<int>(100 * std::sin(static_cast<double>(i) * 0.05));
            data[i] = static_cast<char>(wave + static_cast<int>(lcg.next() >> 30));
        }
        if (c.lds) {
            // Whole groups of five bytes hold 10-bit values; the tail is anything.
            for (qsizetype i = 0; i + 5 <= data.size(); i += 5) {
                data[i + 1] = static_cast<char>(data[i + 1] & 0xFF);
            }
        }
        const QString path = dir.filePath(c.name);
        writeFile(path, data);
        const QString archive = dir.filePath(QString("%1.rfx").arg(c.name));
        rfx::packCaptures({path}, archive, testOptions());
        const QString out = dir.filePath(QString("out_%1").arg(c.name));
        rfx::unpackArchive(archive, out, rfx::UnpackOptions());
        CHECK(readAll(QDir(out).filePath(c.name)) == data);
    }
}

void testTwoCapturesWithReference()
{
    QTemporaryDir dir;
    const size_t samples = 3000000;
    const QByteArray a = duplicatorCapture(samples, 0, 1);
    const QByteArray b = duplicatorCapture(samples, 5, 2);
    writeFile(dir.filePath("a.ddd.s16"), a);
    writeFile(dir.filePath("b.ddd.s16"), b);
    writeFile(dir.filePath("a.ddd.yaml"), "{\n  \"sample_rate_hz\": 40000000\n}\n");
    // 75 frames of two fields of 20 lines 1000 samples apart cover 3 million
    // samples; capture b is a shifted by 5 samples, so its lines are 5 earlier.
    QDir().mkpath(dir.filePath("p0"));
    writeFile(dir.filePath("p0/pass0_0.txt"), pass0Dump(75, 20, 1000, 500));
    writeFile(dir.filePath("p0/pass0_1.txt"), pass0Dump(75, 20, 1000, 495));

    rfx::PackOptions options = testOptions();
    options.pass0Dir = dir.filePath("p0");
    const QString archive = dir.filePath("two.rfx");
    const std::vector<rfx::PackedTrack> report = rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, archive, options);
    CHECK(report.size() == 2);
    {
        rfx::Archive opened(archive);
        CHECK(opened.meta().sources.size() == 2);
        CHECK(opened.meta().sources[0].refs.empty());
        CHECK(opened.meta().sources[1].refs == std::vector<size_t>{0});
        CHECK(opened.meta().sources[0].rateHz == 40000000.0);
        // The second capture is mostly the first: it costs clearly less.
        CHECK(report[1].packed < report[0].packed);
        CHECK(opened.meta().sources[0].shift == 6);
    }
    const QString out = dir.filePath("out");
    rfx::unpackArchive(archive, out, rfx::UnpackOptions());
    CHECK(readAll(QDir(out).filePath("a.ddd.s16")) == a);
    CHECK(readAll(QDir(out).filePath("b.ddd.s16")) == b);
    CHECK(readAll(QDir(out).filePath("a.ddd.yaml")) == readAll(dir.filePath("a.ddd.yaml")));

    // On a machine with an OpenCL GPU: the same archive from the GPU, byte for byte,
    // and the same captures out of it.
    try {
        rfx::cl::Device::get();
        rfx::PackOptions gpuOptions = options;
        gpuOptions.device = rfx::Device::Gpu;
        const QString onGpu = dir.filePath("two_gpu.rfx");
        rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, onGpu, gpuOptions);
        CHECK(readAll(onGpu) == readAll(archive));
        rfx::UnpackOptions gpuUnpack;
        gpuUnpack.device = rfx::Device::Gpu;
        rfx::unpackArchive(archive, dir.filePath("out_gpu"), gpuUnpack);
        CHECK(readAll(dir.filePath("out_gpu/a.ddd.s16")) == a);
        CHECK(readAll(dir.filePath("out_gpu/b.ddd.s16")) == b);
    } catch (const rfx::Error &e) {
        std::fprintf(stderr, "no GPU test: %s\n", e.what());
    }

    // Only the second track: the first is decoded as its reference and removed.
    const QString only = dir.filePath("only");
    rfx::UnpackOptions one;
    one.only = 1;
    rfx::unpackArchive(archive, only, one);
    CHECK(readAll(QDir(only).filePath("b.ddd.s16")) == b);
    CHECK(!QFile::exists(QDir(only).filePath("a.ddd.s16")));

    // Editing: the first track alone, copied as it is.
    const QString first = dir.filePath("first.rfx");
    rfx::assembleArchives({archive}, rfx::pickSome(archive, {0}), first, testOptions(), std::nullopt);
    rfx::unpackArchive(first, dir.filePath("out_first"), rfx::UnpackOptions());
    CHECK(readAll(dir.filePath("out_first/a.ddd.s16")) == a);

    // Merging an archive with itself would repeat the names: refused.
    bool refused = false;
    try {
        rfx::assembleArchives({archive, first}, rfx::pickAll({archive, first}), dir.filePath("merged.rfx"), testOptions(), std::nullopt);
    } catch (const rfx::Error &) {
        refused = true;
    }
    CHECK(refused);
    CHECK(!QFile::exists(dir.filePath("merged.rfx")));

    // A damaged archive is refused, not decoded into something else.
    QByteArray damaged = readAll(archive);
    damaged[damaged.size() / 2] = static_cast<char>(damaged[damaged.size() / 2] ^ 0x55);
    writeFile(dir.filePath("damaged.rfx"), damaged);
    bool caught = false;
    try {
        rfx::unpackArchive(dir.filePath("damaged.rfx"), dir.filePath("out_damaged"), rfx::UnpackOptions());
    } catch (const rfx::Error &) {
        caught = true;
    }
    CHECK(caught);
}

} // namespace

// What is changed in a track on the way: its name, rate and note without coding anything, a track coded again at a level of
// its own, and a capture added with its note.
void testEditedTracks()
{
    QTemporaryDir dir;
    const size_t samples = 3000000;
    const QByteArray a = duplicatorCapture(samples, 0, 1);
    const QByteArray b = duplicatorCapture(samples, 5, 2);
    const QByteArray c = duplicatorCapture(samples, 9, 3);
    writeFile(dir.filePath("a.ddd.s16"), a);
    writeFile(dir.filePath("b.ddd.s16"), b);
    writeFile(dir.filePath("c.ddd.s16"), c);
    writeFile(dir.filePath("a.ddd.yaml"), "{\n  \"sample_rate_hz\": 40000000\n}\n");
    QDir().mkpath(dir.filePath("p0"));
    writeFile(dir.filePath("p0/pass0_0.txt"), pass0Dump(75, 20, 1000, 500));
    writeFile(dir.filePath("p0/pass0_1.txt"), pass0Dump(75, 20, 1000, 495));
    writeFile(dir.filePath("p0/pass0_2.txt"), pass0Dump(75, 20, 1000, 491));
    rfx::PackOptions options = testOptions();
    options.pass0Dir = dir.filePath("p0");
    const QString archive = dir.filePath("two.rfx");
    rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, archive, options);
    {
        // Packed: the date of each file is in the metadata, and there is no note.
        rfx::Archive opened(archive);
        CHECK(opened.meta().sources[0].fileTime.has_value());
        CHECK(opened.meta().sources[0].notes.empty());
    }

    // Name, rate, note and the sidecar files, with nothing coded: the blocks are copied as they are.
    {
        std::vector<rfx::Pick> picks = rfx::pickAll({archive});
        picks[0].edit.notes = std::string("first capture\nPioneer");
        picks[1].edit.name = std::string("second.ddd.s16");
        picks[1].edit.rateHz = 30e6;
        picks[1].edit.sidecars = std::vector<rfx::Sidecar>{};
        picks[0].edit.sidecars = std::vector<rfx::Sidecar>{};
        const QString edited = dir.filePath("edited.rfx");
        rfx::assembleArchives({archive}, picks, edited, options, std::nullopt);
        rfx::Archive opened(edited);
        CHECK(opened.meta().sources.size() == 2);
        CHECK(opened.meta().sources[0].notes == "first capture\nPioneer");
        CHECK(opened.meta().sources[1].name == "second.ddd.s16");
        CHECK(opened.meta().sources[1].rateHz == 30e6);
        CHECK(opened.meta().sources[0].sidecars.empty());
        CHECK(opened.meta().sources[1].refs == std::vector<size_t>{0});
        CHECK(opened.meta().sources[0].fileTime == rfx::Archive(archive).meta().sources[0].fileTime);
        rfx::unpackArchive(edited, dir.filePath("out_edited"), rfx::UnpackOptions());
        CHECK(readAll(dir.filePath("out_edited/a.ddd.s16")) == a);
        CHECK(readAll(dir.filePath("out_edited/second.ddd.s16")) == b);
        // The note can be taken away again.
        std::vector<rfx::Pick> again = rfx::pickAll({edited});
        again[0].edit.notes = std::string();
        const QString cleared = dir.filePath("cleared.rfx");
        rfx::assembleArchives({edited}, again, cleared, options, std::nullopt);
        CHECK(rfx::Archive(cleared).meta().sources[0].notes.empty());
    }

    // A track coded again at a level of its own, and a capture added with its note and level; what comes out is what went in.
    {
        std::vector<rfx::Pick> picks = rfx::pickAll({archive});
        picks[1].edit.recode = true;
        picks[1].edit.level = 4;
        picks.push_back(rfx::Pick::fresh(dir.filePath("c.ddd.s16")));
        picks.back().edit.notes = std::string("added later");
        picks.back().edit.level = 2;
        picks.back().edit.rateHz = 40e6;
        const QString more = dir.filePath("more.rfx");
        rfx::assembleArchives({archive}, picks, more, options, std::nullopt);
        rfx::Archive opened(more);
        CHECK(opened.meta().sources.size() == 3);
        CHECK(opened.meta().sources[2].notes == "added later");
        CHECK(opened.meta().sources[2].rateHz == 40e6);
        CHECK(opened.meta().sources[2].fileTime.has_value());
        CHECK(opened.meta().sources[1].own != opened.meta().sources[2].own || opened.meta().sources[1].taps != opened.meta().sources[2].taps);
        rfx::unpackArchive(more, dir.filePath("out_more"), rfx::UnpackOptions());
        CHECK(readAll(dir.filePath("out_more/a.ddd.s16")) == a);
        CHECK(readAll(dir.filePath("out_more/b.ddd.s16")) == b);
        CHECK(readAll(dir.filePath("out_more/c.ddd.s16")) == c);
    }
}

int main()

{
    testEveryFormat();
    testTwoCapturesWithReference();
    testEditedTracks();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("rfx-roundtrip-tests: ok");
    return 0;
}
