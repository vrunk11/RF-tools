/******************************************************************************
 * gpupipeline.h
 * rfx library - the GPU path of pack and unpack, as a pipeline whose stages run at
 * once on successive chunks of segments, so that neither the device nor the CPU
 * waits on the other
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "archive.h"
#include "metadata.h"
#include "posfile.h"
#include "refwindow.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace rfx {

// Pack track `k`: read the samples and reference windows (CPU, parallel), predict
// (GPU), then residuals, range coding, self-check and writing (CPU). `sink(segment,
// bytes)` receives the coded segments in order. Returns the bytes coded.
uint64_t gpuPackTrack(size_t k, const ArchiveMeta &meta, const std::vector<PosFile *> &files, const ReferenceWindows &windows, bool verify,
                      unsigned threads, const std::function<void(uint64_t, const std::vector<uint8_t> &)> &sink);

// Unpack track `k` of `archive` into `out` (segment q at sample q * segment); the
// reference tracks are read through `windows` (already written).
void gpuUnpackTrack(size_t k, Archive &archive, const ReferenceWindows &windows, PosFile &out, unsigned threads);

} // namespace rfx
