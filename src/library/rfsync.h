/******************************************************************************
 * rfsync.h
 * rfx library - pass 0 straight from the RF: where the lines of a capture are,
 * and the frame codes of its fields, with no ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "alignment.h"
#include "trackreader.h"

#include <atomic>
#include <cstddef>
#include <vector>

namespace rfx {

// The video system the lines are located for.
enum class VideoSystem { Ntsc, Pal };

// Locate every line of track `track` of `reader`, from its RF, without demodulating it (a port of the `rfsync` module of
// ld-decode-rust, the pass 0 of its read-time TBC): the carrier is lowest during a sync pulse, so a coarse frequency estimate
// finds every pulse; the pulse lengths tell the horizontal sync from the equalizing and broad pulses of the vertical
// interval, which gives each field and its parity; each line is then refined to a fraction of a sample on the colour burst
// (NTSC) or the pilot (PAL); the Philips codes of the lines after each vertical interval give the frame codes.
//
// `fs` is the sample rate in Hz and `threads` the threads the filters run on (0: one per processor). The fields come in the
// form of a pass 0 dump (`Field`), so that the lag tables are built from them as from a file. `stop`, when given, is polled:
// when it becomes true the work ends and what was found so far is returned. Throws rfx::Error.
std::vector<Field> locateFields(TrackReader &reader, size_t track, double fs, VideoSystem system, unsigned threads,
                                const std::atomic<bool> *stop = nullptr);

// The length of a line in samples at `fs` on the disc's nominal speed.
double nominalLineSamples(double fs, VideoSystem system);

// A pass 0 dump of the fields, as ld-decode writes it (`LD_DUMP_PASS0`), exact to the bit: for tests, and to look at.
std::string formatPass0(const std::vector<Field> &fields);

} // namespace rfx
