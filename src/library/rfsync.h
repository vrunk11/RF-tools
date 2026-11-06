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
#include <functional>
#include <map>
#include <optional>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace rfx {

// The video system the lines are located for.
enum class VideoSystem { Ntsc, Pal };

// The system a standard says (the `standard` of a track: PAL and SECAM are 625 lines, PAL-M, NTSC and NTSC-VHD are 525); nothing when it says nothing.
std::optional<VideoSystem> systemOfStandard(const std::string &standard);

// The systems to try for a track, in order: the one its standard says first, and the other after it in case what was said is wrong; NTSC then
// PAL when it says nothing.
std::array<VideoSystem, 2> systemsToTry(const std::string &standard);

// A quick look at the start of a track (a few per cent of it): which system its lines are of. The first stretch is located as each system (the
// one `standard` says first) and the one that finds more fields is the answer; nothing when neither finds any (or `stop` was set). The
// long work of locating the lines of the whole track can then start with the right system, and the person can be told at once that the
// standard said for the track seems wrong.
std::optional<VideoSystem> detectSystem(TrackReader &reader, size_t track, double fs, const std::string &standard, const std::atomic<bool> *stop = nullptr);

// The standard to suggest when the one said for a track does not agree with the system its lines were found in (`foundPal`): "pal" or "ntsc".
// Nothing when no standard is said, or it agrees: only what seems wrong is asked about.
std::optional<std::string> doubtedStandard(const std::string &standard, bool foundPal);

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
                                const std::atomic<bool> *stop = nullptr,
                                const std::function<void(double)> &progress = {});

// The length of a line in samples at `fs` on the disc's nominal speed.
double nominalLineSamples(double fs, VideoSystem system);

// The quick look of keepPass0 on its own: for the tracks keepPass0 would work on (the same arguments), the system their lines are of, found from the
// start of each (detectSystem); a track in which neither system finds lines is not in the result. Nothing is written. Used to ask about the standard
// of the tracks before the long work starts. Nothing for a file that is not of a LaserDisc (see laserdiscWork).
std::map<size_t, VideoSystem> lookAtSystems(const QString &archive, double fallbackRateMhz, bool onlyMissing, const std::atomic<bool> *stop = nullptr,
                                            std::optional<size_t> track = std::nullopt, bool assumeLaserdisc = false);

// Locate the lines of the tracks of an archive from their RF and keep what is found in the archive (specification 3.1: a block added at the
// end of the file, nothing else rewritten), from which the speed of the disc and the time base correction are made. The tracks are worked on
// together, so that what they are coded against is decoded once. A quick look at the start of each (detectSystem) tells the system its lines are of,
// the standard said for the track and PAL or NTSC after it otherwise. With onlyMissing, the tracks the archive
// already has the lines of are left as they are. Only a LaserDisc has a TBC curve: for a file whose source is something else it throws, and for one that
// says nothing it throws unless `assumeLaserdisc` (the person said to treat it as one). `fallbackRateMhz` is the rate of a track that has none. Progress goes to stderr as `pass 0`.
// `track`: that one only (the lines of the others, kept already, are left as they are). Returns the number of tracks that were done. Throws rfx::Error.
size_t keepPass0(const QString &archive, double fallbackRateMhz, bool onlyMissing, const std::atomic<bool> *stop = nullptr,
                 std::optional<size_t> track = std::nullopt, bool assumeLaserdisc = false);

// A pass 0 dump of the fields, as ld-decode writes it (`LD_DUMP_PASS0`), exact to the bit: for tests, and to look at.
std::string formatPass0(const std::vector<Field> &fields);

} // namespace rfx
