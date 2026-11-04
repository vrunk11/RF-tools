/******************************************************************************
 * library_tests.cpp
 * rfx library - unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "ebml.h"
#include "error.h"
#include "lagtable.h"
#include "metadata.h"
#include "posfile.h"
#include "predictor.h"
#include "rangecoder.h"
#include "residualmodel.h"
#include "sampleformat.h"
#include "segment.h"
#include "sha256.h"
#include "yaml.h"

#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
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

#define CHECK_EQ(a, b)                                                                                              \
    do {                                                                                                            \
        if (!((a) == (b))) {                                                                                        \
            std::fprintf(stderr, "%s:%d: %s differs from %s\n", __FILE__, __LINE__, #a, #b);                       \
            ++failures;                                                                                             \
        }                                                                                                           \
    } while (0)

// The generator the reference's tests use.
struct Lcg
{
    uint32_t x;
    uint32_t next()
    {
        x = x * 1664525u + 1013904223u;
        return x;
    }
};

std::string sha256Hex(const std::vector<uint8_t> &bytes)
{
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

// SHA-256 with the processor's instructions and without, against Qt's, over lengths
// that cross every padding case and over input given in uneven pieces.
void testSha256()
{
    {
        rfx::Sha256 empty;
        CHECK_EQ(empty.finishHex(), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        rfx::Sha256 abc;
        abc.update("abc", 3);
        CHECK_EQ(abc.finishHex(), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }
    Lcg lcg{2024};
    std::vector<uint8_t> data(70000);
    for (uint8_t &b : data) {
        b = static_cast<uint8_t>(lcg.next() >> 24);
    }
    for (const size_t length : {size_t(0), size_t(1), size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(119), size_t(120), size_t(128), size_t(1000), size_t(69999)}) {
        const QByteArray want = QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(data.data()), static_cast<qsizetype>(length)), QCryptographicHash::Sha256).toHex();
        for (const bool hardware : {false, true}) {
            rfx::Sha256 h(hardware);
            // Uneven pieces: 7, 64, 129, then the rest.
            size_t at = 0;
            for (const size_t piece : {size_t(7), size_t(64), size_t(129), length}) {
                const size_t take = std::min(piece, length - at);
                h.update(data.data() + at, take);
                at += take;
            }
            CHECK_EQ(h.finishHex(), want.toStdString());
        }
    }
    std::fprintf(stderr, "sha256: processor instructions %s\n", rfx::Sha256::hardwareAvailable() ? "used" : "not available");
}

void testVint()
{
    using rfx::ebml::vint;
    CHECK_EQ(vint(0), (std::vector<uint8_t>{0x80}));
    CHECK_EQ(vint(126), (std::vector<uint8_t>{0xFE}));
    CHECK_EQ(vint(127), (std::vector<uint8_t>{0x40, 0x7F}));
    CHECK_EQ(vint(16382), (std::vector<uint8_t>{0x7F, 0xFE}));
    CHECK_EQ(vint(16383), (std::vector<uint8_t>{0x20, 0x3F, 0xFF}));
}

void testRangeCoderRoundTrip()
{
    std::vector<rfx::P2> probs(4);
    rfx::RangeEncoder enc;
    Lcg lcg{12345};
    struct Step
    {
        uint32_t bit;
        size_t prob;
        uint32_t value;
    };
    std::vector<Step> want;
    for (size_t i = 0; i < 20000; ++i) {
        const uint32_t x = lcg.next();
        const Step s{((x >> 20) % 7 == 0) ? 1u : 0u, i % 4, x >> 28};
        want.push_back(s);
        enc.bit(probs[s.prob], s.bit);
        enc.direct(s.value, 4);
    }
    const std::vector<uint8_t> bytes = enc.finish();
    std::vector<rfx::P2> again(4);
    rfx::RangeDecoder dec(bytes.data(), bytes.size());
    bool same = true;
    for (const Step &s : want) {
        same = same && dec.bit(again[s.prob]) == s.bit && dec.direct(4) == s.value;
    }
    CHECK(same);
    CHECK(!dec.overrun());
}

void testResidualModelRoundTrip()
{
    Lcg lcg{7};
    std::vector<int32_t> values;
    for (int i = 0; i < 50000; ++i) {
        const int s = i % 5000 < 2500 ? 3 : 400;
        values.push_back(static_cast<int32_t>((lcg.next() >> 16) % (2 * s + 1)) - s);
    }
    for (const int32_t v : {0, 1, -1, 1 << 20, -(1 << 20)}) {
        values.push_back(v);
    }
    const std::vector<uint8_t> bytes = rfx::encodeResiduals(values);
    std::vector<int32_t> back;
    CHECK(rfx::decodeResiduals(bytes.data(), bytes.size(), values.size(), back));
    CHECK_EQ(back, values);
}

void testLagTableRoundTrip()
{
    rfx::LagTable t;
    uint64_t pos = 0;
    int64_t lag = -37;
    Lcg lcg{99};
    for (int i = 0; i < 2000; ++i) {
        t.pos.push_back(pos);
        t.lag.push_back(lag);
        pos += 64 * (1 + lcg.next() % 6000);
        const uint32_t r = lcg.next() % 100;
        lag += r < 90 ? 1 : (r < 97 ? -1 : static_cast<int64_t>(lcg.next() % 100000) - 50000);
    }
    const std::vector<uint8_t> coded = t.encode();
    const rfx::LagTable back = rfx::LagTable::decode(coded.data(), coded.size());
    CHECK_EQ(back.pos, t.pos);
    CHECK_EQ(back.lag, t.lag);
    CHECK_EQ(t.lagAt(-5), t.lag[0]);
    CHECK_EQ(t.lagAt(static_cast<int64_t>(t.pos[10])), t.lag[10]);
    CHECK_EQ(t.lagAt(static_cast<int64_t>(t.pos[10]) - 1), t.lag[9]);

    std::vector<uint8_t> varints;
    for (const uint64_t v : {uint64_t(0), uint64_t(127), uint64_t(128), uint64_t(300), ~uint64_t(0) >> 1}) {
        rfx::putVarint(varints, v);
    }
    size_t at = 0;
    for (const uint64_t v : {uint64_t(0), uint64_t(127), uint64_t(128), uint64_t(300), ~uint64_t(0) >> 1}) {
        CHECK_EQ(rfx::getVarint(varints.data(), varints.size(), at), v);
    }
}

void testSampleFormats()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    struct Case
    {
        rfx::SampleFormat fmt;
        int32_t lo;
        int32_t hi;
        int32_t scale;
    };
    const Case cases[] = {
        {rfx::SampleFormat::S16, -32768, 32767, 1}, {rfx::SampleFormat::U16, 0, 65535, 1}, {rfx::SampleFormat::S8, -128, 127, 1},
        {rfx::SampleFormat::U8, 0, 255, 1},         {rfx::SampleFormat::Lds, -512, 511, 64},
    };
    for (const Case &c : cases) {
        const size_t n = 4096;
        Lcg lcg{9};
        std::vector<int32_t> vals(n);
        for (int32_t &v : vals) {
            v = (c.lo + static_cast<int32_t>((lcg.next() >> 8) % static_cast<uint32_t>(c.hi - c.lo + 1))) * c.scale;
        }
        const QString path = dir.filePath(QString("t.%1").arg(rfx::formatName(c.fmt)));
        rfx::PosFile file(path, rfx::PosFile::Mode::Write);
        rfx::storeSamples(c.fmt, file, 0, vals.data(), 2048);
        rfx::storeSamples(c.fmt, file, 2048, vals.data() + 2048, n - 2048);
        CHECK_EQ(rfx::formatSamples(c.fmt, file.size()), n);
        std::vector<int32_t> back(100);
        rfx::loadSamples(c.fmt, file, n, 1001, back.data(), back.size());
        CHECK_EQ(back, std::vector<int32_t>(vals.begin() + 1001, vals.begin() + 1101));
        std::vector<int32_t> edge(10, 7);
        rfx::loadSamples(c.fmt, file, n, static_cast<int64_t>(n) - 4, edge.data(), edge.size());
        CHECK_EQ(std::vector<int32_t>(edge.begin(), edge.begin() + 4), std::vector<int32_t>(vals.end() - 4, vals.end()));
        CHECK_EQ(std::vector<int32_t>(edge.begin() + 4, edge.end()), std::vector<int32_t>(6, 0));
    }
    // 7 bytes of an .lds file: one group of four samples and a tail of two bytes.
    CHECK_EQ(rfx::formatSamples(rfx::SampleFormat::Lds, 7), 4u);
    CHECK_EQ(rfx::formatCovered(rfx::SampleFormat::Lds, 7), 5u);
}

void testEbmlScan()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("t.rfx");
    {
        rfx::PosFile file(path, rfx::PosFile::Mode::Write);
        std::vector<uint8_t> all = rfx::ebml::fileHead();
        auto add = [&all](const std::vector<uint8_t> &part) { all.insert(all.end(), part.begin(), part.end()); };
        add(rfx::ebml::info());
        add(rfx::ebml::tracks({rfx::ebml::trackEntry(1, "D_RFX/PRED", "a"), rfx::ebml::trackEntry(2, "D_RFX/META", "m")}));
        add(rfx::ebml::cluster(0, 2, {'h', 'e', 'l', 'l', 'o'}));
        add(rfx::ebml::cluster(7, 1, std::vector<uint8_t>(300, 1)));
        file.writeAt(all.data(), all.size(), 0);
    }
    rfx::PosFile file(path, rfx::PosFile::Mode::Read);
    const rfx::ebml::Index idx = rfx::ebml::scan(file, std::string("rfx"));
    CHECK_EQ(idx.codecs.size(), 2u);
    CHECK_EQ(idx.codecs[0].second, std::string("D_RFX/PRED"));
    CHECK_EQ(idx.blocks.size(), 2u);
    CHECK_EQ(idx.blocks[0].track, 2u);
    CHECK_EQ(idx.blocks[0].len, 5u);
    CHECK_EQ(idx.blocks[1].track, 1u);
    CHECK_EQ(idx.blocks[1].ts, 7u);
    CHECK_EQ(idx.blocks[1].len, 300u);
    char hello[5];
    file.readAt(hello, 5, idx.blocks[0].off);
    CHECK(std::memcmp(hello, "hello", 5) == 0);
    bool refused = false;
    try {
        rfx::ebml::scan(file, std::string("matroska"));
    } catch (const rfx::Error &) {
        refused = true;
    }
    CHECK(refused);
}

void testMetadata()
{
    rfx::ArchiveMeta m;
    m.segment = 262144;
    rfx::SourceMeta a;
    a.name = "a.s16";
    a.size = 10;
    a.words = 5;
    a.sha256 = "00";
    a.shift = 6;
    a.mean = -3;
    a.own = 16;
    a.solo = 32;
    a.taps = 6;
    a.stride = 1;
    a.every = 16;
    a.lamBits = 1065336832;
    rfx::SourceMeta b = a;
    b.name = "b.s16";
    b.refs = {0};
    b.weights = {0.3f};
    b.ntop = 1;
    b.ttaps = 1;
    b.noise = 0.25;
    b.origin = std::string("b.ldf");
    b.rateHz = 75000000.0;
    b.tail = {1};
    b.size = 11;
    b.sidecars.push_back({"b.yaml", "ff", {'x', '\n', 0xC3, 0xA9}});
    m.sources = {a, b};
    // The note and the date of the file go through the metadata, and are not written when they are not there (so that an
    // archive without them is the same bytes as it always was).
    {
        rfx::ArchiveMeta plain = m;
        for (rfx::SourceMeta &s : plain.sources) {
            s.notes.clear();
            s.fileTime.reset();
        }
        const std::vector<uint8_t> bare = rfx::serializeMeta(plain);
        CHECK(std::string(bare.begin(), bare.end()).find("\"notes\"") == std::string::npos);
        CHECK(std::string(bare.begin(), bare.end()).find("\"file_time\"") == std::string::npos);
        rfx::ArchiveMeta noted = plain;
        noted.sources[0].notes = "Side 1, \"first\" capture\nPioneer LD-1100 \xC3\xA9";
        noted.sources[0].fileTime = "2023-05-16T18:03:54Z";
        const std::vector<uint8_t> withNote = rfx::serializeMeta(noted);
        const rfx::ArchiveMeta again = rfx::parseMeta(withNote.data(), withNote.size());
        CHECK(again.sources[0].notes == noted.sources[0].notes);
        CHECK(again.sources[0].fileTime && *again.sources[0].fileTime == "2023-05-16T18:03:54Z");
        CHECK(again.sources.size() < 2 || (again.sources[1].notes.empty() && !again.sources[1].fileTime));
    }
    const std::vector<uint8_t> json = rfx::serializeMeta(m);
    const rfx::ArchiveMeta back = rfx::parseMeta(json.data(), json.size());
    CHECK_EQ(back.sources.size(), 2u);
    CHECK_EQ(back.sources[1].refs, std::vector<size_t>{0});
    CHECK_EQ(back.sources[1].weights.size(), 1u);
    CHECK(back.sources[1].weights[0] == 0.3f);
    CHECK(back.sources[1].noise && *back.sources[1].noise == 0.25);
    CHECK(back.sources[1].origin && *back.sources[1].origin == "b.ldf");
    CHECK(back.sources[1].rateHz == 75000000.0);
    CHECK_EQ(back.sources[1].sidecars[0].data, (std::vector<uint8_t>{'x', '\n', 0xC3, 0xA9}));
    CHECK_EQ(back.sources[0].mean, -3);
    CHECK_EQ(back.sources[1].tail, (std::vector<uint8_t>{1}));

    // A track that is predicted from a later one is refused.
    m.sources[0].refs = {1};
    const std::vector<uint8_t> loop = rfx::serializeMeta(m);
    bool refused = false;
    try {
        rfx::parseMeta(loop.data(), loop.size());
    } catch (const rfx::Error &) {
        refused = true;
    }
    CHECK(refused);
}

void testYamlNumbers()
{
    rfx::Node doc = rfx::Node::object();
    doc.set("rate", rfx::Node::real(75000000.0));
    doc.set("ratio", rfx::Node::real(0.30000001192092896));
    doc.set("small", rfx::Node::real(0.001));
    doc.set("big", rfx::Node::real(1.5e20));
    doc.set("zero", rfx::Node::real(0.0));
    doc.set("text", rfx::Node::string("a\nb"));
    doc.set("empty", rfx::Node::array());
    doc.set("list", rfx::Node::array().push(rfx::Node::unsignedInteger(1)).push(rfx::Node::integer(-2)));
    std::string out;
    doc.writeYaml(0, out);
    CHECK_EQ(out,
             std::string("big: 1.5e20\nempty: []\nlist: [1, -2]\nrate: 75000000.0\nratio: 0.30000001192092896\nsmall: 0.001\ntext: |\n  a\n  b\nzero: 0.0\n"));
}

// What the reference (the Rust crate) codes for these segments: the bytes are
// compared by their SHA-256, and the lengths. The signal is made by the
// generator below from a fixed seed; see AGENTS.md section 5.
struct Golden
{
    int level;
    bool useRef;
    size_t length;
    const char *sha256;
};

const Golden kGolden[] = {
    {1, false, 6100, "b23fc44146e41960c09e50479a7003a6dfd6868c7552c519d1deaf33be66a858"},
    {1, true, 6282, "aee4fb9150240ef76d6e63220d5d22ff0d47c4f316aca1955532c62291765c6f"},
    {2, false, 6082, "204a1c05b8db1bc3005c6dc1f74f9caee45a61080d88b9d61bf734c2c493b4b7"},
    {2, true, 2557, "bd14381cfd9cf1d7c43965c3677bbaf10de16c32936f48304b0f5bd3deec8191"},
    {3, false, 6030, "e62cbf7a0fbd2698fca84d36149d7659d384f5207396d044f6d551c5ee25ad04"},
    {3, true, 2481, "f708b89b0b29f61c075881c425c8966938286d7b936dc208d6766d2f6d4211bc"},
    {4, false, 6046, "25f660af2317cb2d295eba9b1da341021ddd67b75ff48f484db0b3242ece5434"},
    {4, true, 2509, "4edb7b40bc957bdde1d59fb1ef4fd2bc2f0044b876f6061ed65e4ffac8296316"},
};

// The forgetting factor of each level, as the reference computes it.
const uint32_t kLamBits[] = {1065222591, 1065287776, 1065336832, 1065336832};

void testPredictorMatchesReference()
{
    constexpr size_t n = 6000;
    for (int level = 1; level <= 4; ++level) {
        rfx::Preset pre;
        CHECK(rfx::Preset::level(level, pre));
        CHECK_EQ(pre.lamBits, kLamBits[level - 1]);
    }
    for (const Golden &g : kGolden) {
        rfx::Preset pre;
        CHECK(rfx::Preset::level(g.level, pre));
        Lcg lcg{12345};
        const size_t l = n + 2 * pre.taps;
        std::vector<int32_t> sig(n + 64);
        for (int32_t &v : sig) {
            v = static_cast<int32_t>((lcg.next() >> 20) % 201) - 100;
        }
        std::vector<int32_t> y(n);
        for (size_t i = 0; i < n; ++i) {
            y[i] = sig[i + 8] + static_cast<int32_t>((lcg.next() >> 24) % 5) - 2;
        }
        std::vector<float> window(pre.channels() * l);
        for (size_t t = 0; t < window.size(); ++t) {
            window[t] = static_cast<float>(sig[(t + (t / l) * 7) % (n + 64)] + static_cast<int32_t>((lcg.next() >> 24) % 5) - 2);
        }
        const std::vector<int32_t> residuals = rfx::predictionErrors(y, pre, g.useRef, window.data(), l);
        const std::vector<uint8_t> bytes = rfx::encodeResiduals(residuals);
        CHECK_EQ(bytes.size(), g.length);
        CHECK_EQ(sha256Hex(bytes), std::string(g.sha256));
        std::vector<int32_t> decoded;
        CHECK(rfx::decodeResiduals(bytes.data(), bytes.size(), n, decoded));
        CHECK_EQ(rfx::reconstruct(decoded, pre, g.useRef, window.data(), l), y);
    }
}

} // namespace

int main()
{
    testSha256();
    testVint();
    testRangeCoderRoundTrip();
    testResidualModelRoundTrip();
    testLagTableRoundTrip();
    testSampleFormats();
    testEbmlScan();
    testMetadata();
    testYamlNumbers();
    testPredictorMatchesReference();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("rfx-library-tests: ok");
    return 0;
}
