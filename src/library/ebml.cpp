/******************************************************************************
 * ebml.cpp
 * rfx library - the little of Matroska the archive uses (specification 2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "ebml.h"

#include "error.h"

#include <algorithm>

namespace rfx {
namespace ebml {

namespace {

constexpr uint64_t kTimestampScaleNs = 1000000;
constexpr uint64_t kTrackTypePrivate = 0x21;
constexpr uint64_t kMaxAttachmentBytes = uint64_t(1) << 28;
constexpr size_t kHeaderPeek = 12;
constexpr size_t kMaxIdLength = 4;

// The ID and size of an element, and how many bytes they take.
struct Header
{
    uint32_t id = 0;
    bool known = false;
    uint64_t size = 0;
    uint64_t length = 0;
};

int leadingZeros(uint8_t b)
{
    int n = 0;
    for (uint8_t mask = 0x80; mask != 0 && (b & mask) == 0; mask >>= 1) {
        ++n;
    }
    return n;
}

Bytes idBytes(uint32_t id)
{
    Bytes out;
    bool started = false;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const uint8_t b = static_cast<uint8_t>(id >> shift);
        if (b != 0 || started) {
            out.push_back(b);
            started = true;
        }
    }
    return out;
}

void append(Bytes &out, const Bytes &more)
{
    out.insert(out.end(), more.begin(), more.end());
}

Header readHeader(PosFile &file, uint64_t pos, uint64_t fileLength)
{
    uint8_t buf[kHeaderPeek] = {};
    const size_t n = static_cast<size_t>(std::min<uint64_t>(fileLength - pos, kHeaderPeek));
    if (n < 2) {
        throw Error("truncated element at byte " + std::to_string(pos));
    }
    file.readAt(buf, n, pos);
    if (buf[0] == 0) {
        throw Error("bad element id at byte " + std::to_string(pos));
    }
    const size_t idl = static_cast<size_t>(leadingZeros(buf[0])) + 1;
    if (idl > kMaxIdLength || idl >= n) {
        throw Error("bad element id at byte " + std::to_string(pos));
    }
    Header h;
    for (size_t i = 0; i < idl; ++i) {
        h.id = (h.id << 8) | buf[i];
    }
    const uint8_t s0 = buf[idl];
    if (s0 == 0) {
        throw Error("bad element size at byte " + std::to_string(pos));
    }
    const size_t sl = static_cast<size_t>(leadingZeros(s0)) + 1;
    if (idl + sl > n) {
        throw Error("truncated element at byte " + std::to_string(pos));
    }
    const uint64_t valueMask = (uint64_t(1) << (8 - sl)) - 1;
    uint64_t v = s0 & valueMask;
    bool allOnes = v == valueMask;
    for (size_t i = idl + 1; i < idl + sl; ++i) {
        v = (v << 8) | buf[i];
        allOnes = allOnes && buf[i] == 0xFF;
    }
    h.known = !allOnes;
    h.size = v;
    h.length = idl + sl;
    return h;
}

uint64_t parseUint(const uint8_t *data, size_t size)
{
    uint64_t v = 0;
    for (size_t i = 0; i < size; ++i) {
        v = (v << 8) | data[i];
    }
    return v;
}

Bytes readPayload(PosFile &file, uint64_t off, uint64_t len)
{
    Bytes v(static_cast<size_t>(len));
    if (len > 0) {
        file.readAt(v.data(), v.size(), off);
    }
    return v;
}

struct Child
{
    uint32_t id;
    size_t begin;
    size_t end;
};

// The children of a master element held in memory.
std::vector<Child> children(const uint8_t *buf, size_t size)
{
    std::vector<Child> out;
    size_t i = 0;
    while (i < size) {
        const uint8_t first = buf[i];
        const size_t idl = static_cast<size_t>(leadingZeros(first)) + 1;
        if (first == 0 || idl > kMaxIdLength || i + idl >= size) {
            throw Error("bad element in a master");
        }
        uint32_t id = 0;
        for (size_t k = 0; k < idl; ++k) {
            id = (id << 8) | buf[i + k];
        }
        const uint8_t s0 = buf[i + idl];
        const size_t sl = static_cast<size_t>(leadingZeros(s0)) + 1;
        if (s0 == 0 || i + idl + sl > size) {
            throw Error("bad element size in a master");
        }
        uint64_t v = s0 & ((uint64_t(1) << (8 - sl)) - 1);
        for (size_t k = i + idl + 1; k < i + idl + sl; ++k) {
            v = (v << 8) | buf[k];
        }
        const size_t start = i + idl + sl;
        if (v > size - start) {
            throw Error("element runs past its master");
        }
        out.push_back({id, start, start + static_cast<size_t>(v)});
        i = start + static_cast<size_t>(v);
    }
    return out;
}

// The block whose element data (track number, timestamp offset, flags, then
// the payload) is `len` bytes at `at`.
BlockRef blockRef(PosFile &file, uint64_t at, uint64_t len, uint64_t ts)
{
    uint8_t head[kHeaderPeek] = {};
    const size_t n = static_cast<size_t>(std::min<uint64_t>(len, kHeaderPeek));
    if (n == 0) {
        throw Error("an empty block");
    }
    file.readAt(head, n, at);
    const size_t tl = static_cast<size_t>(leadingZeros(head[0])) + 1;
    const uint64_t skip = tl + 3;
    if (head[0] == 0 || skip > len) {
        throw Error("a damaged block header");
    }
    uint64_t track = head[0] & ((uint64_t(1) << (8 - tl)) - 1);
    for (size_t i = 1; i < tl; ++i) {
        track = (track << 8) | head[i];
    }
    BlockRef b;
    b.track = track;
    b.ts = ts;
    b.off = at + skip;
    b.len = len - skip;
    return b;
}

} // namespace

Bytes vint(uint64_t v)
{
    size_t n = 1;
    while (n < 8 && v >= (uint64_t(1) << (7 * n)) - 1) {
        ++n;
    }
    Bytes out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(v >> (8 * (n - 1 - i)));
    }
    out[0] |= static_cast<uint8_t>(1u << (8 - n));
    return out;
}

Bytes element(uint32_t id, const Bytes &payload)
{
    Bytes out = idBytes(id);
    append(out, vint(payload.size()));
    append(out, payload);
    return out;
}

Bytes uintElement(uint32_t id, uint64_t v)
{
    Bytes b;
    bool started = false;
    for (int shift = 56; shift >= 0; shift -= 8) {
        const uint8_t byte = static_cast<uint8_t>(v >> shift);
        if (byte != 0 || started || shift == 0) {
            b.push_back(byte);
            started = true;
        }
    }
    return element(id, b);
}

Bytes stringElement(uint32_t id, const std::string &s)
{
    return element(id, Bytes(s.begin(), s.end()));
}

Bytes fileHead()
{
    Bytes header;
    append(header, uintElement(0x4286, 1));
    append(header, uintElement(0x42F7, 1));
    append(header, uintElement(0x42F2, 4));
    append(header, uintElement(0x42F3, 8));
    append(header, stringElement(kIdDocType, "rfx"));
    append(header, uintElement(0x4287, 1));
    append(header, uintElement(0x4285, 1));
    Bytes out = element(kIdEbml, header);
    append(out, idBytes(kIdSegment));
    const uint8_t unknownSize[] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    out.insert(out.end(), unknownSize, unknownSize + sizeof(unknownSize));
    return out;
}

Bytes info()
{
    Bytes body;
    append(body, uintElement(kIdTimestampScale, kTimestampScaleNs));
    append(body, stringElement(kIdMuxingApp, "rfx"));
    append(body, stringElement(kIdWritingApp, "rfx"));
    return element(kIdInfo, body);
}

Bytes trackEntry(uint64_t number, const std::string &codec, const std::string &name)
{
    Bytes body;
    append(body, uintElement(kIdTrackNumber, number));
    append(body, uintElement(kIdTrackUid, number));
    append(body, uintElement(kIdTrackType, kTrackTypePrivate));
    append(body, stringElement(kIdCodecId, codec));
    append(body, stringElement(kIdName, name));
    return element(kIdTrackEntry, body);
}

Bytes tracks(const std::vector<Bytes> &entries)
{
    Bytes body;
    for (const Bytes &e : entries) {
        append(body, e);
    }
    return element(kIdTracks, body);
}

Bytes cluster(uint64_t ts, uint64_t track, const Bytes &payload)
{
    Bytes block = vint(track);
    block.push_back(0);
    block.push_back(0);
    block.push_back(0x80);
    append(block, payload);
    Bytes body = uintElement(kIdTimestamp, ts);
    append(body, element(kIdSimpleBlock, block));
    return element(kIdCluster, body);
}

Index scan(PosFile &file, const std::optional<std::string> &docType)
{
    const uint64_t fileLength = file.size();
    Index idx;
    if (fileLength < 2) {
        throw Error("not an rfx file (too short)");
    }
    const Header h = readHeader(file, 0, fileLength);
    if (h.id != kIdEbml) {
        throw Error("not an rfx file (no EBML header)");
    }
    const uint64_t headSize = h.known ? h.size : 0;
    if (headSize > fileLength - h.length) {
        throw Error("the file is truncated (EBML header)");
    }
    const Bytes head = readPayload(file, h.length, headSize);
    for (const Child &c : children(head.data(), head.size())) {
        if (c.id == kIdDocType) {
            idx.docType.assign(reinterpret_cast<const char *>(head.data()) + c.begin, c.end - c.begin);
        }
    }
    if (docType && idx.docType != *docType) {
        throw Error("not an rfx file (DocType is not " + *docType + ")");
    }
    uint64_t pos = h.length + headSize;
    const Header seg = readHeader(file, pos, fileLength);
    if (seg.id != kIdSegment) {
        throw Error("no Segment after the EBML header");
    }
    pos += seg.length;
    const uint64_t segEnd = seg.known ? std::min(pos + seg.size, fileLength) : fileLength;
    while (pos < segEnd) {
        const Header e = readHeader(file, pos, fileLength);
        if (!e.known) {
            throw Error("unknown-size element inside the Segment at byte " + std::to_string(pos));
        }
        const uint64_t body = pos + e.length;
        if (e.size > fileLength - body) {
            throw Error("the file is truncated (element at byte " + std::to_string(pos) + " runs past the end)");
        }
        if (e.id == kIdTracks) {
            const Bytes buf = readPayload(file, body, e.size);
            for (const Child &entry : children(buf.data(), buf.size())) {
                if (entry.id != kIdTrackEntry) {
                    continue;
                }
                uint64_t number = 0;
                std::string codec;
                Bytes priv;
                for (const Child &c : children(buf.data() + entry.begin, entry.end - entry.begin)) {
                    const uint8_t *data = buf.data() + entry.begin + c.begin;
                    const size_t size = c.end - c.begin;
                    if (c.id == kIdTrackNumber) {
                        number = parseUint(data, size);
                    } else if (c.id == kIdCodecId) {
                        codec.assign(reinterpret_cast<const char *>(data), size);
                    } else if (c.id == kIdCodecPrivate) {
                        priv.assign(data, data + size);
                    }
                }
                idx.codecs.emplace_back(number, codec);
                if (!priv.empty()) {
                    idx.codecPrivate.emplace_back(number, priv);
                }
            }
        } else if (e.id == kIdCluster) {
            uint64_t ts = 0;
            uint64_t p = body;
            while (p < body + e.size) {
                const Header c = readHeader(file, p, fileLength);
                const uint64_t csize = c.known ? c.size : 0;
                if (c.id == kIdTimestamp) {
                    const Bytes v = readPayload(file, p + c.length, csize);
                    ts = parseUint(v.data(), v.size());
                } else if (c.id == kIdSimpleBlock) {
                    idx.blocks.push_back(blockRef(file, p + c.length, csize, ts));
                } else if (c.id == kIdBlockGroup) {
                    // A tool like ffmpeg writes a group for the last packet of a
                    // track, or for one with a duration.
                    uint64_t q = p + c.length;
                    while (q < p + c.length + csize) {
                        const Header g = readHeader(file, q, fileLength);
                        const uint64_t gsize = g.known ? g.size : 0;
                        if (g.id == kIdBlock) {
                            idx.blocks.push_back(blockRef(file, q + g.length, gsize, ts));
                        }
                        q += g.length + gsize;
                    }
                }
                p += c.length + csize;
            }
        } else if (e.id == kIdAttachments) {
            if (e.size > kMaxAttachmentBytes) {
                throw Error("the attachments are too large");
            }
            const Bytes buf = readPayload(file, body, e.size);
            for (const Child &f : children(buf.data(), buf.size())) {
                if (f.id != kIdAttachedFile) {
                    continue;
                }
                Attachment a;
                for (const Child &c : children(buf.data() + f.begin, f.end - f.begin)) {
                    const size_t begin = f.begin + c.begin;
                    const size_t end = f.begin + c.end;
                    if (c.id == kIdFileName) {
                        a.name.assign(reinterpret_cast<const char *>(buf.data()) + begin, end - begin);
                    } else if (c.id == kIdFileData) {
                        a.off = body + begin;
                        a.len = end - begin;
                    }
                }
                idx.attachments.push_back(a);
            }
        }
        pos = body + e.size;
    }
    return idx;
}

} // namespace ebml
} // namespace rfx
