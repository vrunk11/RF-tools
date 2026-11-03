/******************************************************************************
 * ebml.h
 * rfx library - the little of Matroska the archive uses (specification 2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "posfile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rfx {
namespace ebml {

constexpr uint32_t kIdEbml = 0x1A45DFA3;
constexpr uint32_t kIdSegment = 0x18538067;
constexpr uint32_t kIdInfo = 0x1549A966;
constexpr uint32_t kIdTracks = 0x1654AE6B;
constexpr uint32_t kIdTrackEntry = 0xAE;
constexpr uint32_t kIdTrackNumber = 0xD7;
constexpr uint32_t kIdTrackUid = 0x73C5;
constexpr uint32_t kIdTrackType = 0x83;
constexpr uint32_t kIdCodecId = 0x86;
constexpr uint32_t kIdCodecPrivate = 0x63A2;
constexpr uint32_t kIdName = 0x536E;
constexpr uint32_t kIdCluster = 0x1F43B675;
constexpr uint32_t kIdTimestamp = 0xE7;
constexpr uint32_t kIdSimpleBlock = 0xA3;
constexpr uint32_t kIdBlockGroup = 0xA0;
constexpr uint32_t kIdBlock = 0xA1;
constexpr uint32_t kIdTimestampScale = 0x2AD7B1;
constexpr uint32_t kIdMuxingApp = 0x4D80;
constexpr uint32_t kIdWritingApp = 0x5741;
constexpr uint32_t kIdAttachments = 0x1941A469;
constexpr uint32_t kIdAttachedFile = 0x61A7;
constexpr uint32_t kIdFileName = 0x466E;
constexpr uint32_t kIdFileData = 0x465C;
constexpr uint32_t kIdDocType = 0x4282;

// ---- writing ----

using Bytes = std::vector<uint8_t>;

// EBML variable-length size, the shortest form.
Bytes vint(uint64_t v);
Bytes element(uint32_t id, const Bytes &payload);
Bytes uintElement(uint32_t id, uint64_t v);
Bytes stringElement(uint32_t id, const std::string &s);
// The EBML header and the start of the Segment (unknown size).
Bytes fileHead();
Bytes info();
// A TrackEntry of the private type 0x21.
Bytes trackEntry(uint64_t number, const std::string &codec, const std::string &name);
Bytes tracks(const std::vector<Bytes> &entries);
// A Cluster holding one SimpleBlock of `track` (timestamp `ts`).
Bytes cluster(uint64_t ts, uint64_t track, const Bytes &payload);

// ---- reading ----

struct BlockRef
{
    uint64_t track = 0;
    uint64_t ts = 0;
    // Where the payload of the block is in the file.
    uint64_t off = 0;
    uint64_t len = 0;
};

struct Attachment
{
    std::string name;
    uint64_t off = 0;
    uint64_t len = 0;
};

struct Index
{
    std::string docType;
    // Codec id of each track number.
    std::vector<std::pair<uint64_t, std::string>> codecs;
    std::vector<BlockRef> blocks;
    // CodecPrivate of each track number that has one.
    std::vector<std::pair<uint64_t, Bytes>> codecPrivate;
    std::vector<Attachment> attachments;
};

// Walk the file once, reading element headers only (no payload but Tracks and
// the first bytes of each block). With `docType`, the file must have it.
// Throws rfx::Error for a file that is not valid.
Index scan(PosFile &file, const std::optional<std::string> &docType);

} // namespace ebml
} // namespace rfx
