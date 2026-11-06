/******************************************************************************
 * metadata.cpp
 * rfx library - the metadata block of an .rfx file (specification 3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "metadata.h"

#include "error.h"
#include "yaml.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace rfx {

namespace {

bool oneOf(const std::string &text, const char *const *values, size_t count, bool lower, std::string &out)
{
    std::string t = text;
    std::transform(t.begin(), t.end(), t.begin(), [lower](unsigned char c) { return static_cast<char>(lower ? std::tolower(c) : std::toupper(c)); });
    for (size_t i = 0; i < count; ++i) {
        std::string v = values[i];
        if (v == t || (!lower && v == t)) {
            out = values[i];
            return true;
        }
    }
    return false;
}

[[noreturn]] void bad(const std::string &what)
{
    throw Error("bad metadata: " + what);
}

const QJsonValue field(const QJsonObject &o, const char *key, bool required)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (required && v.isUndefined()) {
        bad(std::string("the field ") + key + " is missing");
    }
    return v;
}

uint64_t unsignedField(const QJsonObject &o, const char *key, bool required, uint64_t fallback = 0)
{
    const QJsonValue v = field(o, key, required);
    if (v.isUndefined() || v.isNull()) {
        return fallback;
    }
    if (!v.isDouble() || v.toDouble() < 0) {
        bad(std::string("the field ") + key + " is not a non-negative number");
    }
    return static_cast<uint64_t>(v.toDouble());
}

std::string stringField(const QJsonObject &o, const char *key, bool required, const std::string &fallback = std::string())
{
    const QJsonValue v = field(o, key, required);
    if (v.isUndefined() || v.isNull()) {
        return fallback;
    }
    if (!v.isString()) {
        bad(std::string("the field ") + key + " is not a string");
    }
    return v.toString().toStdString();
}

std::vector<uint8_t> byteArray(const QJsonValue &v, const char *key)
{
    std::vector<uint8_t> out;
    if (v.isUndefined() || v.isNull()) {
        return out;
    }
    if (!v.isArray()) {
        bad(std::string("the field ") + key + " is not an array");
    }
    const QJsonArray a = v.toArray();
    out.reserve(static_cast<size_t>(a.size()));
    for (const QJsonValue &e : a) {
        const double d = e.toDouble(-1);
        if (d < 0 || d > 255) {
            bad(std::string("the field ") + key + " holds a value that is not a byte");
        }
        out.push_back(static_cast<uint8_t>(d));
    }
    return out;
}

SourceMeta parseSource(const QJsonObject &o)
{
    SourceMeta s;
    s.name = stringField(o, "name", true);
    s.size = unsignedField(o, "size", true);
    s.sha256 = stringField(o, "sha256", true);
    s.words = unsignedField(o, "words", true);
    s.tail = byteArray(field(o, "tail", true), "tail");
    s.fmt = stringField(o, "fmt", false, "s16");
    s.sampleSha256 = stringField(o, "sample_sha256", false);
    s.shift = static_cast<uint32_t>(unsignedField(o, "shift", true));
    const QJsonValue mean = field(o, "mean", true);
    if (!mean.isDouble()) {
        bad("the field mean is not a number");
    }
    s.mean = static_cast<int32_t>(mean.toDouble());
    s.own = static_cast<size_t>(unsignedField(o, "own", true));
    s.solo = static_cast<size_t>(unsignedField(o, "solo", false));
    s.taps = static_cast<size_t>(unsignedField(o, "taps", true));
    s.ntop = static_cast<size_t>(unsignedField(o, "ntop", false));
    s.ttaps = static_cast<size_t>(unsignedField(o, "ttaps", false));
    s.stride = static_cast<size_t>(unsignedField(o, "stride", true));
    s.every = static_cast<size_t>(unsignedField(o, "every", true));
    s.lamBits = static_cast<uint32_t>(unsignedField(o, "lam_bits", true));
    const QJsonValue refs = field(o, "refs", true);
    if (!refs.isArray()) {
        bad("the field refs is not an array");
    }
    for (const QJsonValue &r : refs.toArray()) {
        if (!r.isDouble() || r.toDouble() < 0) {
            bad("the field refs holds a value that is not a track number");
        }
        s.refs.push_back(static_cast<size_t>(r.toDouble()));
    }
    const QJsonValue weights = field(o, "weights", false);
    if (weights.isArray()) {
        for (const QJsonValue &w : weights.toArray()) {
            s.weights.push_back(static_cast<float>(w.toDouble()));
        }
    }
    s.position = static_cast<size_t>(unsignedField(o, "position", false));
    const QJsonValue noise = field(o, "noise", false);
    if (noise.isDouble()) {
        s.noise = noise.toDouble();
    }
    const QJsonValue sidecars = field(o, "sidecars", false);
    if (sidecars.isArray()) {
        for (const QJsonValue &e : sidecars.toArray()) {
            const QJsonObject so = e.toObject();
            Sidecar sc;
            sc.name = stringField(so, "name", true);
            sc.sha256 = stringField(so, "sha256", true);
            sc.data = byteArray(field(so, "data", true), "data");
            s.sidecars.push_back(std::move(sc));
        }
    }
    const QJsonValue origin = field(o, "origin", false);
    if (origin.isString()) {
        s.origin = origin.toString().toStdString();
    }
    const QJsonValue rate = field(o, "rate_hz", false);
    if (rate.isDouble()) {
        s.rateHz = rate.toDouble();
    }
    const QJsonValue notes = field(o, "notes", false);
    if (notes.isString()) {
        s.notes = notes.toString().toStdString();
    }
    const QJsonValue bits = field(o, "bits", false);
    if (bits.isDouble() && bits.toDouble() > 0) {
        s.bits = static_cast<uint32_t>(bits.toDouble());
    }
    const auto text = [&o](const char *key) {
        const QJsonValue v = field(o, key, false);
        return v.isString() ? v.toString().toStdString() : std::string();
    };
    s.standard = text("standard");
    s.reference = text("reference");
    s.face = text("face");
    s.signal = text("signal");
    const QJsonValue captureNumber = field(o, "capture", false);
    if (captureNumber.isDouble() && captureNumber.toDouble() > 0) {
        s.capture = static_cast<uint32_t>(captureNumber.toDouble());
    }
    const QJsonValue copy = field(o, "copy", false);
    if (copy.isDouble() && copy.toDouble() > 0) {
        s.copy = static_cast<uint32_t>(copy.toDouble());
    }
    const QJsonValue fileTime = field(o, "file_time", false);
    if (fileTime.isString()) {
        s.fileTime = fileTime.toString().toStdString();
    }
    return s;
}

// The document is written by hand, in the order of the fields of the
// specification and as serde_json writes it (no spaces, the shortest digits), so
// that the same archive is the same bytes whichever tool wrote it.
void putBytes(std::string &out, const std::vector<uint8_t> &bytes)
{
    out += '[';
    for (size_t i = 0; i < bytes.size(); ++i) {
        out += (i ? "," : "") + std::to_string(bytes[i]);
    }
    out += ']';
}

void putSource(std::string &out, const SourceMeta &s)
{
    out += "{\"name\":" + jsonQuote(s.name);
    out += ",\"size\":" + std::to_string(s.size);
    out += ",\"sha256\":" + jsonQuote(s.sha256);
    out += ",\"words\":" + std::to_string(s.words);
    out += ",\"tail\":";
    putBytes(out, s.tail);
    out += ",\"fmt\":" + jsonQuote(s.fmt);
    out += ",\"sample_sha256\":" + jsonQuote(s.sampleSha256);
    out += ",\"shift\":" + std::to_string(s.shift);
    out += ",\"mean\":" + std::to_string(s.mean);
    out += ",\"own\":" + std::to_string(s.own);
    out += ",\"solo\":" + std::to_string(s.solo);
    out += ",\"taps\":" + std::to_string(s.taps);
    out += ",\"ntop\":" + std::to_string(s.ntop);
    out += ",\"ttaps\":" + std::to_string(s.ttaps);
    out += ",\"stride\":" + std::to_string(s.stride);
    out += ",\"every\":" + std::to_string(s.every);
    out += ",\"lam_bits\":" + std::to_string(s.lamBits);
    out += ",\"refs\":[";
    for (size_t i = 0; i < s.refs.size(); ++i) {
        out += (i ? "," : "") + std::to_string(s.refs[i]);
    }
    out += "],\"weights\":[";
    for (size_t i = 0; i < s.weights.size(); ++i) {
        out += (i ? "," : "") + formatReal(s.weights[i]);
    }
    out += "],\"position\":" + std::to_string(s.position);
    out += ",\"noise\":" + (s.noise ? formatReal(*s.noise) : std::string("null"));
    out += ",\"sidecars\":[";
    for (size_t i = 0; i < s.sidecars.size(); ++i) {
        const Sidecar &sc = s.sidecars[i];
        out += (i ? ",{\"name\":" : "{\"name\":") + jsonQuote(sc.name);
        out += ",\"sha256\":" + jsonQuote(sc.sha256) + ",\"data\":";
        putBytes(out, sc.data);
        out += '}';
    }
    out += "],\"origin\":" + (s.origin ? jsonQuote(*s.origin) : std::string("null"));
    out += ",\"rate_hz\":" + formatReal(s.rateHz);
    // Written only when there is one, so that an archive without them is the same bytes as before.
    if (!s.notes.empty()) {
        out += ",\"notes\":" + jsonQuote(s.notes);
    }
    if (s.bits > 0) {
        out += ",\"bits\":" + std::to_string(s.bits);
    }
    if (s.fileTime) {
        out += ",\"file_time\":" + jsonQuote(*s.fileTime);
    }
    if (!s.standard.empty()) {
        out += ",\"standard\":" + jsonQuote(s.standard);
    }
    if (!s.reference.empty()) {
        out += ",\"reference\":" + jsonQuote(s.reference);
    }
    if (s.copy > 0) {
        out += ",\"copy\":" + std::to_string(s.copy);
    }
    if (s.capture > 0) {
        out += ",\"capture\":" + std::to_string(s.capture);
    }
    if (!s.face.empty()) {
        out += ",\"face\":" + jsonQuote(s.face);
    }
    if (!s.signal.empty()) {
        out += ",\"signal\":" + jsonQuote(s.signal);
    }
    out += '}';
}

} // namespace

SampleFormat SourceMeta::format() const
{
    SampleFormat f = SampleFormat::S16;
    parseFormat(fmt, f);
    return f;
}

Preset SourceMeta::preset() const
{
    Preset p;
    p.own = own;
    p.solo = solo == 0 ? own : solo;
    p.plain = own;
    p.taps = taps;
    p.ntop = ntop;
    p.ttaps = ttaps;
    p.stride = stride;
    p.every = every;
    p.lamBits = lamBits;
    return p;
}

std::vector<size_t> SourceMeta::tops() const
{
    std::vector<size_t> order(refs.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    if (weights.size() == refs.size()) {
        std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) { return weights[a] > weights[b]; });
    }
    std::vector<size_t> out;
    for (size_t i = 0; i < order.size() && i < ntop; ++i) {
        out.push_back(refs[order[i]]);
    }
    return out;
}

ArchiveMeta parseMeta(const uint8_t *data, size_t size)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size)), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        bad(err.error != QJsonParseError::NoError ? err.errorString().toStdString() : "not an object");
    }
    const QJsonObject root = doc.object();
    ArchiveMeta m;
    m.format = stringField(root, "format", true);
    m.version = static_cast<uint32_t>(unsignedField(root, "version", true));
    if (m.format != kFormatName || m.version != kFormatVersion) {
        throw Error("unsupported rfx version " + std::to_string(m.version));
    }
    m.segment = static_cast<size_t>(unsignedField(root, "segment", true));
    if (m.segment == 0) {
        bad("the segment size is 0");
    }
    {
        // (a value that is none of the media is read as not said: it is not for this reader to refuse a file for it)
        const QJsonValue medium = field(root, "medium", false);
        if (medium.isString()) {
            parseMedium(medium.toString().toStdString(), m.medium);
        }
    }
    const QJsonValue sources = field(root, "sources", true);
    if (!sources.isArray()) {
        bad("the field sources is not an array");
    }
    for (const QJsonValue &s : sources.toArray()) {
        m.sources.push_back(parseSource(s.toObject()));
    }
    for (size_t k = 0; k < m.sources.size(); ++k) {
        const SourceMeta &s = m.sources[k];
        for (const size_t r : s.refs) {
            if (r >= k) {
                bad("a track is predicted from a track that is not before it");
            }
        }
        if (!s.weights.empty() && s.weights.size() != s.refs.size()) {
            bad("a track has a weight for only some of its references");
        }
        SampleFormat f;
        if (!parseFormat(s.fmt, f)) {
            bad("unknown sample format " + s.fmt);
        }
        if (formatSamples(f, s.size) != s.words || s.tail.size() != s.size - formatCovered(f, s.size)) {
            bad("the size of " + s.name + " does not match its samples");
        }
        const bool powers = s.stride != 0 && (s.stride & (s.stride - 1)) == 0 && s.every != 0 && (s.every & (s.every - 1)) == 0;
        if (!powers || s.own == 0 || s.shift > 31 || s.preset().dimension(!s.refs.empty()) > Predictor::kMaxDimension) {
            bad("the predictor of " + s.name + " is not valid");
        }
        if (!s.refs.empty() && s.taps < s.ttaps) {
            bad("the predictor of " + s.name + " is not valid");
        }
    }
    return m;
}

std::vector<uint8_t> serializeMeta(const ArchiveMeta &meta)
{
    std::string out = "{\"format\":" + jsonQuote(meta.format);
    out += ",\"version\":" + std::to_string(meta.version);
    out += ",\"segment\":" + std::to_string(meta.segment);
    if (!meta.medium.empty()) {
        out += ",\"medium\":" + jsonQuote(meta.medium);
    }
    out += ",\"sources\":[";
    for (size_t i = 0; i < meta.sources.size(); ++i) {
        if (i) {
            out += ',';
        }
        putSource(out, meta.sources[i]);
    }
    out += "]}";
    return std::vector<uint8_t>(out.begin(), out.end());
}

std::vector<SourceMeta> parseFlacMeta(const uint8_t *data, size_t size, std::string *codecOut, std::string *mediumOut)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size)), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        bad(err.error != QJsonParseError::NoError ? err.errorString().toStdString() : "not an object");
    }
    const QJsonObject root = doc.object();
    const std::string format = stringField(root, "format", true);
    const std::string codec = stringField(root, "codec", true);
    const uint32_t version = static_cast<uint32_t>(unsignedField(root, "version", true));
    if (format != kFormatName || (codec != "flac" && codec != "raw") || version != kFormatVersion) {
        throw Error("unsupported FLAC or raw archive (" + format + " " + codec + " v" + std::to_string(version) + ")");
    }
    if (codecOut) {
        *codecOut = codec;
    }
    if (mediumOut) {
        mediumOut->clear();
        const QJsonValue medium = field(root, "medium", false);
        if (medium.isString()) {
            parseMedium(medium.toString().toStdString(), *mediumOut);
        }
    }
    const QJsonValue tracks = field(root, "tracks", true);
    if (!tracks.isArray()) {
        bad("the field tracks is not an array");
    }
    std::vector<SourceMeta> out;
    for (const QJsonValue &t : tracks.toArray()) {
        SourceMeta s = parseSource(t.toObject());
        SampleFormat f;
        if (!parseFormat(s.fmt, f)) {
            bad("unknown sample format " + s.fmt);
        }
        if (formatSamples(f, s.size) != s.words || s.tail.size() != s.size - formatCovered(f, s.size)) {
            bad("the size of " + s.name + " does not match its samples");
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<uint8_t> serializeFlacMeta(const std::vector<SourceMeta> &tracks, const char *codec, const std::string &medium)
{
    std::string out = std::string("{\"format\":\"rfx\",\"codec\":\"") + codec + "\",\"version\":1";
    if (!medium.empty()) {
        out += ",\"medium\":" + jsonQuote(medium);
    }
    out += ",\"tracks\":[";
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (i) {
            out += ',';
        }
        putSource(out, tracks[i]);
    }
    out += "]}";
    return std::vector<uint8_t>(out.begin(), out.end());
}

std::string toHex(const uint8_t *bytes, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}

bool parseStandard(const std::string &text, std::string &out)
{
    return oneOf(text, kStandards, sizeof(kStandards) / sizeof(kStandards[0]), true, out);
}

bool parseMedium(const std::string &text, std::string &out)
{
    std::string t;
    for (const char c : text) {
        t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (t.empty() || t == "unknown") {
        out.clear();
        return true;
    }
    return oneOf(t, kMedia, sizeof(kMedia) / sizeof(kMedia[0]), true, out);
}

std::string mediumName(const std::string &medium)
{
    std::string out;
    for (const char c : medium) {
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out.empty() ? "unknown" : out;
}

std::string mergedMedium(const std::vector<std::string> &media)
{
    std::string said;
    for (const std::string &m : media) {
        if (m.empty()) {
            continue;
        }
        if (said.empty()) {
            said = m;
        } else if (said != m) {
            return std::string();
        }
    }
    return said;
}

bool parseFace(const std::string &text, std::string &out)
{
    return oneOf(text, kFaces, sizeof(kFaces) / sizeof(kFaces[0]), false, out);
}

bool parseSignal(const std::string &text, std::string &out)
{
    // ("Y/C" and "YC" are the same)
    std::string t;
    for (const char c : text) {
        if (c != '/' && c != '-' && c != ' ') {
            t += c;
        }
    }
    return oneOf(t, kSignals, sizeof(kSignals) / sizeof(kSignals[0]), true, out);
}

std::string describeInfo(const SourceMeta &sm)
{
    std::string out;
    const auto add = [&out](const std::string &name, const std::string &value) {
        out += (out.empty() ? "" : ", ") + name + " " + value;
    };
    if (!sm.standard.empty()) {
        add("standard", sm.standard);
    }
    if (!sm.reference.empty()) {
        add("reference", jsonQuote(sm.reference));
    }
    if (sm.copy > 0) {
        add("disc copy", std::to_string(sm.copy));
    }
    if (sm.capture > 0) {
        add("capture", std::to_string(sm.capture));
    }
    if (!sm.face.empty()) {
        add("face", sm.face);
    }
    if (!sm.signal.empty()) {
        add("signal", sm.signal == "yc" ? "Y/C" : "CVBS");
    }
    return out;
}
void InfoEdit::overlay(const InfoEdit &over)
{
    if (over.standard) {
        standard = over.standard;
    }
    if (over.reference) {
        reference = over.reference;
    }
    if (over.copy) {
        copy = over.copy;
    }
    if (over.capture) {
        capture = over.capture;
    }
    if (over.face) {
        face = over.face;
    }
    if (over.signal) {
        signal = over.signal;
    }
}

InfoEdit InfoEdit::of(const SourceMeta &sm)
{
    InfoEdit e;
    e.standard = sm.standard;
    e.reference = sm.reference;
    e.copy = sm.copy;
    e.capture = sm.capture;
    e.face = sm.face;
    e.signal = sm.signal;
    return e;
}

void SourceMeta::apply(const InfoEdit &edit)
{
    if (edit.standard) {
        standard = *edit.standard;
    }
    if (edit.reference) {
        reference = *edit.reference;
    }
    if (edit.copy) {
        copy = *edit.copy;
    }
    if (edit.capture) {
        capture = *edit.capture;
    }
    if (edit.face) {
        face = *edit.face;
    }
    if (edit.signal) {
        signal = *edit.signal;
    }
}
} // namespace rfx
