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
#include "convert.h"
#include "flacmode.h"
#include "opencl.h"
#include "packer.h"
#include "rfsync.h"
#include "sampleformat.h"
#include "trackreader.h"
#include "verify.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
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

// The raw variant: the samples as they are, as PCM tracks of a Matroska file; and the FLAC variant beside it (both need ffmpeg).
void testStoredVariants()
{
    QTemporaryDir dir;
    const QByteArray a = duplicatorCapture(200000, 0, 1);
    const QByteArray b = duplicatorCapture(200000, 3, 2);
    writeFile(dir.filePath("a.ddd.s16"), a);
    writeFile(dir.filePath("b.ddd.s16"), b);
    writeFile(dir.filePath("a.ddd.yaml"), "{\n  \"sample_rate_hz\": 40000000\n}\n");
    rfx::PackOptions options = testOptions();
    options.rateMhz = 40.0;
    for (const rfx::StoredCodec codec : {rfx::StoredCodec::Raw, rfx::StoredCodec::Flac}) {
        const bool raw = codec == rfx::StoredCodec::Raw;
        const QString file = dir.filePath(raw ? "raw.rfx" : "flac.rfx");
        try {
            rfx::packFlac({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, file, options, codec);
        } catch (const rfx::Error &e) {
            std::puts("rfx-roundtrip-tests: no ffmpeg, the Matroska variants are not tested");
            return;
        }
        CHECK(rfx::isFlacArchive(file));
        std::string stored;
        const std::vector<rfx::SourceMeta> tracks = rfx::readFlacMeta(file, &stored);
        CHECK(stored == (raw ? "raw" : "flac"));
        CHECK(tracks.size() == 2);
        CHECK(tracks[0].rateHz == 40e6);
        CHECK(tracks[0].fileTime.has_value());
        CHECK(rfx::flacInfo(file).find(raw ? "raw PCM" : "FLAC") != std::string::npos);
        // The raw file holds the samples as they are (and so is about as large as they are); the FLAC one is smaller.
        const qint64 size = QFileInfo(file).size();
        CHECK(raw ? size > a.size() + b.size() : size < a.size() + b.size());
        const QString out = dir.filePath(raw ? "out_raw" : "out_flac");
        rfx::unpackFlac(file, out, std::nullopt, std::nullopt);
        CHECK(readAll(out + "/a.ddd.s16") == a);
        CHECK(readAll(out + "/b.ddd.s16") == b);
        rfx::exportMultichannelFlac(file, dir.filePath(raw ? "mflac_raw" : "mflac_flac"), std::nullopt);
        CHECK(QFileInfo(dir.filePath(raw ? "mflac_raw" : "mflac_flac") + "/" + QFileInfo(file).completeBaseName() + ".flac").size() > 0);

        // The editing commands work on the variant and keep it: extract, remove, merge, add, and a note put on a track.
        const QString onlyA = dir.filePath(raw ? "only_a_raw.rfx" : "only_a_flac.rfx");
        const QString onlyB = dir.filePath(raw ? "only_b_raw.rfx" : "only_b_flac.rfx");
        rfx::assembleArchives({file}, rfx::pickSome(file, {0}), onlyA, options, std::nullopt);
        rfx::assembleArchives({file}, rfx::pickWithout(file, {0}), onlyB, options, std::nullopt);
        std::string codecName;
        CHECK(rfx::readFlacMeta(onlyA, &codecName).size() == 1 && codecName == (raw ? "raw" : "flac"));
        CHECK(rfx::readFlacMeta(onlyB).at(0).name == "b.ddd.s16");
        const QString merged = dir.filePath(raw ? "merged_raw.rfx" : "merged_flac.rfx");
        rfx::assembleArchives({onlyB, onlyA}, rfx::pickAll({onlyB, onlyA}), merged, options, std::nullopt);
        const std::vector<rfx::SourceMeta> mergedTracks = rfx::readFlacMeta(merged);
        CHECK(mergedTracks.size() == 2 && mergedTracks[0].name == "b.ddd.s16" && mergedTracks[1].name == "a.ddd.s16");
        CHECK(mergedTracks[1].fileTime == tracks[0].fileTime && mergedTracks[1].rateHz == 40e6);
        std::vector<rfx::Pick> more = rfx::pickAll({file});
        writeFile(dir.filePath("c.ddd.s16"), a);
        more.push_back(rfx::Pick::fresh(dir.filePath("c.ddd.s16")));
        more[1].edit.notes = std::string("the second");
        more[1].edit.name = std::string("second.s16");
        const QString added = dir.filePath(raw ? "added_raw.rfx" : "added_flac.rfx");
        rfx::assembleArchives({file}, more, added, options, std::nullopt);
        const std::vector<rfx::SourceMeta> addedTracks = rfx::readFlacMeta(added);
        CHECK(addedTracks.size() == 3 && addedTracks[2].name == "c.ddd.s16");
        CHECK(addedTracks[1].notes == "the second" && addedTracks[1].name == "second.s16");
        rfx::unpackFlac(added, dir.filePath(raw ? "out_added_raw" : "out_added_flac"), std::nullopt, std::nullopt);
        CHECK(readAll(dir.filePath(raw ? "out_added_raw/second.s16" : "out_added_flac/second.s16")) == b);
    }
}

// What a track keeps when the capture is coded again against its archive (as rf-requantize does): what was said of the disc, the number of the
// capture included, and its TBC curve, which belongs to the track.
void testLikeKeepsTheTrack()
{
    QTemporaryDir dir;
    const size_t samples = 3000000;
    const QByteArray a = duplicatorCapture(samples, 0, 1);
    const QByteArray b = duplicatorCapture(samples, 5, 2);
    writeFile(dir.filePath("a.ddd.s16"), a);
    writeFile(dir.filePath("b.ddd.s16"), b);
    writeFile(dir.filePath("a.ddd.yaml"), "{\n  \"sample_rate_hz\": 40000000\n}\n");
    QDir().mkpath(dir.filePath("p0"));
    writeFile(dir.filePath("p0/pass0_0.txt"), pass0Dump(75, 20, 1000, 500));
    writeFile(dir.filePath("p0/pass0_1.txt"), pass0Dump(75, 20, 1000, 495));
    rfx::PackOptions options = testOptions();
    options.pass0Dir = dir.filePath("p0");
    options.keepPass0 = true;
    options.medium = std::string("ld");
    options.info.standard = std::string("pal");
    options.info.reference = std::string("LD-1234");
    options.info.copy = 2u;
    options.info.capture = 3u;
    options.info.face = std::string("BA");
    options.info.signal = std::string("yc");
    const QString archive = dir.filePath("two.rfx");
    rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, archive, options);
    const rfx::Archive original(archive);
    CHECK(original.pass0().size() == 2);

    // The second track alone, coded again: it is track 0 of the new file, and keeps its disc and its curve.
    const QString again = dir.filePath("again.rfx");
    rfx::packCapturesLike({dir.filePath("b.ddd.s16")}, archive, {1}, again, testOptions(), std::nullopt);
    const rfx::Archive opened(again);
    CHECK(opened.meta().sources.size() == 1);
    const rfx::SourceMeta &sm = opened.meta().sources[0];
    CHECK(sm.standard == "pal");
    CHECK(sm.reference == "LD-1234");
    CHECK(sm.copy == 2);
    CHECK(sm.capture == 3);
    CHECK(sm.face == "BA");
    CHECK(sm.signal == "yc");
    CHECK(opened.pass0().size() == 1);
    if (!opened.pass0().empty() && original.pass0().count(1) != 0) {
        const rfx::SavedPass0 &kept = opened.pass0().begin()->second;
        const rfx::SavedPass0 &was = original.pass0().at(1);
        CHECK(opened.pass0().begin()->first == 0);
        CHECK(was.pal);   // (the standard said for the captures is what locates their lines as PAL)
        CHECK(kept.pal == was.pal);
        CHECK(kept.fields.size() == was.fields.size());
        CHECK(!kept.fields.empty() && !was.fields.empty() && kept.fields.front().lines == was.fields.front().lines);
    }
}

// The standard of a track tells which video system its lines are located for first.
void testSystemOfStandard()
{
    using rfx::VideoSystem;
    CHECK(rfx::systemOfStandard("pal") == VideoSystem::Pal);
    CHECK(rfx::systemOfStandard("secam") == VideoSystem::Pal);
    for (const char *s : {"ntsc", "pal-m", "ntsc-vhd"}) {
        CHECK(rfx::systemOfStandard(s) == VideoSystem::Ntsc);
    }
    CHECK(!rfx::systemOfStandard("").has_value());
    CHECK(!rfx::systemOfStandard("unknown").has_value());
    // Only what seems wrong is doubted: a standard that agrees, or none, is not.
    CHECK(rfx::doubtedStandard("pal", false) == std::string("ntsc"));
    CHECK(rfx::doubtedStandard("secam", false) == std::string("ntsc"));
    CHECK(rfx::doubtedStandard("ntsc", true) == std::string("pal"));
    CHECK(rfx::doubtedStandard("pal-m", true) == std::string("pal"));
    CHECK(!rfx::doubtedStandard("pal", true).has_value());
    CHECK(!rfx::doubtedStandard("ntsc-vhd", false).has_value());
    CHECK(!rfx::doubtedStandard("", true).has_value());
    CHECK(!rfx::doubtedStandard("", false).has_value());
    // Said: that one first and the other after. Not said: NTSC, then PAL.
    CHECK(rfx::systemsToTry("pal")[0] == VideoSystem::Pal && rfx::systemsToTry("pal")[1] == VideoSystem::Ntsc);
    CHECK(rfx::systemsToTry("pal-m")[0] == VideoSystem::Ntsc && rfx::systemsToTry("pal-m")[1] == VideoSystem::Pal);
    CHECK(rfx::systemsToTry("")[0] == VideoSystem::Ntsc && rfx::systemsToTry("")[1] == VideoSystem::Pal);
}

// The program itself: `check` says which tracks are intact, and `merge` takes the tracks named after a file.
void testCommandLine()
{
    QTemporaryDir dir;
    const size_t samples = 1500000;
    writeFile(dir.filePath("a.ddd.s16"), duplicatorCapture(samples, 0, 1));
    writeFile(dir.filePath("b.ddd.s16"), duplicatorCapture(samples, 5, 2));
    writeFile(dir.filePath("c.ddd.s16"), duplicatorCapture(samples, 9, 3));
    const QString ab = dir.filePath("ab.rfx");
    const QString c = dir.filePath("c.rfx");
    rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, ab, testOptions());
    rfx::packCaptures({dir.filePath("c.ddd.s16")}, c, testOptions());

    const auto run = [](const QStringList &args, QString *out = nullptr) {
        QProcess p;
        p.start(QStringLiteral(RFX_PROGRAM), args);
        if (!p.waitForFinished(120000)) {
            p.kill();
            return -1;
        }
        if (out) {
            *out = QString::fromUtf8(p.readAllStandardOutput());
        }
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    };

    // check: every track intact, several files at once, then one byte changed in the samples of the file
    QString said;
    CHECK(run({QStringLiteral("check"), ab, c}, &said) == 0);
    CHECK(said.count(QStringLiteral(": intact")) == 3);
    CHECK(run({QStringLiteral("check"), ab, QStringLiteral("--track"), QStringLiteral("1")}, &said) == 0);
    CHECK(said.count(QStringLiteral(": intact")) == 1);
    CHECK(run({QStringLiteral("check"), QStringLiteral("--track"), QStringLiteral("x"), ab}) == 2);
    CHECK(run({QStringLiteral("check")}) == 2);
    {
        const QString broken = dir.filePath("broken.rfx");
        QByteArray bytes = readAll(ab);
        bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x55);
        writeFile(broken, bytes);
        CHECK(run({QStringLiteral("check"), broken, c}, &said) == 1);
        CHECK(said.contains(QStringLiteral("NOT intact")));
        CHECK(said.contains(QStringLiteral(": intact")));   // (the other file is still said intact)
    }

    // merge with tracks named: the second of the first file and the only one of the second, no track coded again
    const QString merged = dir.filePath("merged.rfx");
    CHECK(run({QStringLiteral("merge"), QStringLiteral("-o"), merged, ab + QStringLiteral(":1"), c}) == 0);
    {
        const rfx::Archive opened(merged);
        CHECK(opened.meta().sources.size() == 2);
        CHECK(opened.meta().sources[0].name == "b.ddd.s16");
        CHECK(opened.meta().sources[1].name == "c.ddd.s16");
        rfx::unpackArchive(merged, dir.filePath("out"), rfx::UnpackOptions());
        CHECK(readAll(dir.filePath("out/b.ddd.s16")) == readAll(dir.filePath("b.ddd.s16")));
        CHECK(readAll(dir.filePath("out/c.ddd.s16")) == readAll(dir.filePath("c.ddd.s16")));
    }
    // a track that is not there is an error, and nothing is written
    const QString none = dir.filePath("none.rfx");
    CHECK(run({QStringLiteral("merge"), QStringLiteral("-o"), none, ab + QStringLiteral(":5"), c}) == 1);
    CHECK(!QFileInfo::exists(none));
    // add takes the tracks named of an .rfx file too
    const QString added = dir.filePath("added.rfx");
    CHECK(run({QStringLiteral("add"), QStringLiteral("-o"), added, c, ab + QStringLiteral(":0")}) == 0);
    {
        const rfx::Archive opened(added);
        CHECK(opened.meta().sources.size() == 2);
        CHECK(opened.meta().sources[1].name == "a.ddd.s16");
    }
    // The source of the file: said with tag (the samples are copied as they are), asked about by info, and refused by the TBC curve unless it is a
    // LaserDisc's (or the capture is told to be treated as one).
    const QString tagged = dir.filePath("tagged.rfx");
    CHECK(run({QStringLiteral("tag"), QStringLiteral("-o"), tagged, QStringLiteral("--source"), QStringLiteral("vhs"), ab}) == 0);
    CHECK(rfx::Archive(tagged).meta().medium == "vhs");
    CHECK(run({QStringLiteral("info"), tagged}, &said) == 0);
    CHECK(said.contains(QStringLiteral("source: VHS")));
    CHECK(run({QStringLiteral("tag"), QStringLiteral("-o"), dir.filePath("bad.rfx"), QStringLiteral("--source"), QStringLiteral("betamax"), ab}) == 2);
    CHECK(run({QStringLiteral("tag"), QStringLiteral("-o"), dir.filePath("nothing.rfx"), ab}) == 2);
    CHECK(run({QStringLiteral("tbc-curve"), tagged}) == 1);   // (a VHS tape has no TBC curve)
    CHECK(run({QStringLiteral("tbc-curve"), ab}) == 1);       // (not said: only when told to treat it as a LaserDisc capture)
    CHECK(run({QStringLiteral("check"), tagged}) == 0);
    const QString merged2 = dir.filePath("merged-ld.rfx");
    CHECK(run({QStringLiteral("merge"), QStringLiteral("-o"), merged2, QStringLiteral("--source"), QStringLiteral("ld"), ab, c}) == 0);
    CHECK(rfx::Archive(merged2).meta().medium == "ld");
}

// What was captured is said of the whole file (a LaserDisc, a VHS tape...): written, kept by what is made from the file, and agreed between files that
// are merged. The TBC curve is for LaserDisc captures: a file of another source (or that says none, without being told to treat it as one) has none.
void testMedium()
{
    // (the values)
    std::string out;
    CHECK(rfx::parseMedium("LD", out) && out == "ld");
    CHECK(rfx::parseMedium("vhs", out) && out == "vhs");
    CHECK(rfx::parseMedium("vhd", out) && out == "vhd");
    CHECK(rfx::parseMedium("CVBS", out) && out == "cvbs");
    CHECK(rfx::parseMedium("unknown", out) && out.empty());
    CHECK(rfx::parseMedium("", out) && out.empty());
    CHECK(!rfx::parseMedium("betamax", out));
    CHECK(rfx::mediumName("") == "unknown" && rfx::mediumName("ld") == "LD");
    CHECK(rfx::mergedMedium({"ld", ""}) == "ld");
    CHECK(rfx::mergedMedium({"", ""}).empty());
    CHECK(rfx::mergedMedium({"ld", "vhs"}).empty());
    CHECK(rfx::laserdiscWork("ld", false) && !rfx::laserdiscWork("", false) && rfx::laserdiscWork("", true) && !rfx::laserdiscWork("vhs", true));

    QTemporaryDir dir;
    const size_t samples = 1500000;
    writeFile(dir.filePath("a.ddd.s16"), duplicatorCapture(samples, 0, 1));
    writeFile(dir.filePath("b.ddd.s16"), duplicatorCapture(samples, 5, 2));
    writeFile(dir.filePath("c.ddd.s16"), duplicatorCapture(samples, 9, 3));
    rfx::PackOptions said = testOptions();
    said.medium = std::string("ld");
    const QString ld = dir.filePath("ld.rfx");
    const QString plain = dir.filePath("plain.rfx");
    const QString tape = dir.filePath("tape.rfx");
    rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, ld, said);
    rfx::packCaptures({dir.filePath("c.ddd.s16")}, plain, testOptions());
    rfx::PackOptions vhs = testOptions();
    vhs.medium = std::string("vhs");
    rfx::packCaptures({dir.filePath("c.ddd.s16")}, tape, vhs);
    CHECK(rfx::Archive(ld).meta().medium == "ld");
    CHECK(rfx::Archive(plain).meta().medium.empty());
    CHECK(rfx::Archive(tape).meta().medium == "vhs");
    CHECK(rfx::archiveInfo(ld).find("source: LD") != std::string::npos);
    CHECK(rfx::archiveInfo(plain).find("source: unknown") != std::string::npos);
    CHECK(rfx::archiveYaml(ld).find("medium: \"ld\"") != std::string::npos);
    CHECK(rfx::archiveYaml(plain).find("medium") == std::string::npos);

    // what is made from files keeps what they say, when they agree
    const auto mediumOf = [](const QString &file) { return rfx::Archive(file).meta().medium; };
    rfx::assembleArchives({ld}, rfx::pickSome(ld, {1}), dir.filePath("extracted.rfx"), testOptions(), std::nullopt);
    CHECK(mediumOf(dir.filePath("extracted.rfx")) == "ld");
    rfx::assembleArchives({ld, plain}, rfx::pickAll({ld, plain}), dir.filePath("ld-plain.rfx"), testOptions(), std::nullopt);
    CHECK(mediumOf(dir.filePath("ld-plain.rfx")) == "ld");   // (a file that says nothing is not against it)
    rfx::assembleArchives({ld, tape}, rfx::pickAll({ld, tape}), dir.filePath("ld-tape.rfx"), testOptions(), std::nullopt);
    CHECK(mediumOf(dir.filePath("ld-tape.rfx")).empty());    // (two sources: none is said)
    // said again, of the whole file: another value, and none
    rfx::PackOptions change = testOptions();
    change.medium = std::string("vhd");
    rfx::assembleArchives({ld}, rfx::pickAll({ld}), dir.filePath("changed.rfx"), change, std::nullopt);
    CHECK(mediumOf(dir.filePath("changed.rfx")) == "vhd");
    change.medium = std::string();
    rfx::assembleArchives({ld}, rfx::pickAll({ld}), dir.filePath("cleared.rfx"), change, std::nullopt);
    CHECK(mediumOf(dir.filePath("cleared.rfx")).empty());
    // coded again against the file (what rf-requantize does): what was captured stays
    rfx::packCapturesLike({dir.filePath("b.ddd.s16")}, ld, {1}, dir.filePath("like.rfx"), testOptions(), std::nullopt);
    CHECK(mediumOf(dir.filePath("like.rfx")) == "ld");
    // (the samples are as they were: the check passes)
    for (const rfx::TrackCheck &c : rfx::verifyArchive(dir.filePath("changed.rfx"))) {
        CHECK(c.ok);
    }

    // The TBC curve is for LaserDisc captures
    QDir().mkpath(dir.filePath("p0"));
    writeFile(dir.filePath("p0/pass0_0.txt"), pass0Dump(75, 20, 1000, 500));
    writeFile(dir.filePath("p0/pass0_1.txt"), pass0Dump(75, 20, 1000, 495));
    rfx::PackOptions curve = testOptions();
    curve.pass0Dir = dir.filePath("p0");
    curve.keepPass0 = true;
    const auto curves = [&](const std::string &medium, bool assume) {
        curve.medium = medium;
        curve.assumeLaserdisc = assume;
        const QString file = dir.filePath(QStringLiteral("curve-%1-%2.rfx").arg(QString::fromStdString(medium.empty() ? "none" : medium)).arg(assume));
        rfx::packCaptures({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, file, curve);
        return rfx::Archive(file).pass0().size();
    };
    CHECK(curves("ld", false) == 2);
    CHECK(curves("", true) == 2);    // (told to treat it as one)
    CHECK(curves("", false) == 0);   // (not said, and not told: no curve)
    CHECK(curves("vhs", true) == 0); // (another source: none, whatever is told)
    // the work on a file that is made refuses what is not a LaserDisc's, and asks to be told for one that does not say
    bool refused = false;
    try {
        rfx::keepPass0(plain, 40.0, true);
    } catch (const rfx::Error &) {
        refused = true;
    }
    CHECK(refused);
    refused = false;
    try {
        rfx::keepPass0(tape, 40.0, true, nullptr, std::nullopt, true);
    } catch (const rfx::Error &) {
        refused = true;
    }
    CHECK(refused);

    // the FLAC and raw variants say it too (they need ffmpeg)
    rfx::PackOptions stored = testOptions();
    stored.rateMhz = 40.0;
    stored.medium = std::string("ld");
    for (const rfx::StoredCodec codec : {rfx::StoredCodec::Raw, rfx::StoredCodec::Flac}) {
        const QString file = dir.filePath(codec == rfx::StoredCodec::Raw ? "raw.rfx" : "flac.rfx");
        try {
            rfx::packFlac({dir.filePath("a.ddd.s16"), dir.filePath("b.ddd.s16")}, file, stored, codec);
        } catch (const rfx::Error &) {
            return;   // (no ffmpeg: said already by the other test)
        }
        std::string medium;
        rfx::readFlacMeta(file, nullptr, &medium);
        CHECK(medium == "ld");
        CHECK(rfx::flacInfo(file).find("source: LD") != std::string::npos);
        CHECK(rfx::TrackReader(file).meta().medium == "ld");
        const QString copy = dir.filePath("stored-copy.rfx");
        rfx::assembleArchives({file}, rfx::pickSome(file, {1}), copy, testOptions(), std::nullopt);
        medium.clear();
        rfx::readFlacMeta(copy, nullptr, &medium);
        CHECK(medium == "ld");
        rfx::PackOptions again = testOptions();
        again.medium = std::string("cvbs");
        rfx::assembleArchives({file}, rfx::pickAll({file}), copy, again, std::nullopt);
        medium.clear();
        rfx::readFlacMeta(copy, nullptr, &medium);
        CHECK(medium == "cvbs");
    }
}

int main()

{
    testEveryFormat();
    testTwoCapturesWithReference();
    testEditedTracks();
    testStoredVariants();
    testLikeKeepsTheTrack();
    testSystemOfStandard();
    testCommandLine();
    testMedium();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("rfx-roundtrip-tests: ok");
    return 0;
}
