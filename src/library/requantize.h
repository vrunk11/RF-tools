/******************************************************************************
 * requantize.h
 * rfx library - requantising a capture offline, as the capture application
 * would have done it: samples in a stream or a file of any capture format, and
 * whole jobs (a capture in, the same capture requantised out; an .rfx file in, an
 * .rfx file out)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools. The stream half is ddd-requantize of the
 * Domesday Duplicator, ported.
 ******************************************************************************/

#pragma once

#include "error.h"
#include "packer.h"
#include "posfile.h"
#include "requantizer.h"
#include "sampleformat.h"

#include <QString>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rfx {

// The command line is wrong, or options that cannot go together were given. The
// tool exits with 2 (AGENTS.md 4.4) rather than 1.
class UsageError : public Error
{
public:
    explicit UsageError(const std::string &message) : Error(message) {}
};

// The bits a sample may carry at the narrowest and the widest.
constexpr int kFewestInputBits = 5;
constexpr int kMostInputBits = 16;

// The width of the converter of the Domesday Duplicator.
constexpr int kConverterBits = 10;

// What a person asks of the requantiser.
struct RequantizeOptions
{
    // False: the samples go through untouched, which is how the reference a
    // comparison is made against is written.
    bool requantize = true;
    int marginLevel = kDefaultMarginLevel;
    std::vector<FrequencyBand> bands;
    bool adaptive = false;
    std::vector<double> depthsDb;
    int order = 0;
    // The bits the input carries; found from the capture, or from its samples.
    std::optional<int> inputBits;
    // Hand the segments on no faster than the rate of the samples, as a capture
    // does, for whatever reads the output live.
    bool realtime = false;
};

// What became of one track.
struct RequantizeResult
{
    std::string name;
    double rateMhz = 0.0;
    int inputBits = 0;
    bool requantized = true;
    RequantizationStatus::Summary summary;
    // Seconds a paced run took, and the seconds of samples it carried.
    double tookSeconds = 0.0;
    double lastsSeconds = 0.0;
};

// Where the segments of a stream come from and go. A reader returns how many
// samples it put in `out` (at most `count`; fewer only at the end), a writer takes
// a segment.
using SegmentReader = std::function<size_t(int16_t *out, size_t count)>;
using SegmentWriter = std::function<void(const int16_t *samples, size_t count)>;

// What `requantizeStream` is told about the stream.
struct StreamSpec
{
    std::string name;
    double rateMhz = 0.0;
    // Bits the input carries when the capture says (its bit shift); otherwise they
    // are found from the first segment.
    std::optional<int> knownBits;
    // The samples there are, for the progress, or 0.
    uint64_t total = 0;
    // Takes a line of the CSV log (one a segment, after requantizeLogHeader()), if
    // set; `logTrack` is its first column.
    std::function<void(const std::string &)> log;
    int logTrack = 0;
    // Called with a percentage when it changes.
    std::function<void(int)> progress;
};

// Segment after segment from the start, 2^20 samples each, the last whatever is
// left: each decided and, where it is being kept, requantised in place before the
// writer takes it. With no writer, only the analysis is paid for.
RequantizeResult requantizeStream(const RequantizeOptions &options, const StreamSpec &spec, const SegmentReader &read,
                                  const SegmentWriter &write);

// The bits a segment's low bits say it carries: a converter's ten, left-aligned,
// leave six zero bits under every sample, and a bit shift one more for each bit
// shifted.
int bitsFoundIn(const std::vector<int16_t> &samples);

// The bits a capture of `fmt` whose samples have `shift` low zero bits (the
// archive's own statement, specification 1) carries on the 16-bit scale.
int bitsOfFormat(SampleFormat fmt, uint32_t shift);

// The first line of the CSV log.
const char *requantizeLogHeader();

// The requantisation as a capture records it, tag by tag: what was asked, as the
// capture application stamps it on a FLAC file (the report shows them).
std::vector<std::pair<std::string, std::string>> requantizationTags(const RequantizeOptions &options, const RequantizerSettings &settings);

// The ends of a stream. A file is read and written by sample index in any capture
// format (a sample the output format cannot hold is an error); standard input and
// output are signed 16-bit little-endian. `words` is the samples the file has, and
// `name` is what an error calls it. Each keeps its own position: make one for each
// pass over a file.
SegmentReader fileReader(PosFile &in, SampleFormat fmt, uint64_t words, const std::string &name);
SegmentWriter fileWriter(PosFile &out, SampleFormat fmt, const std::string &name);
SegmentReader standardReader(std::FILE *in);
SegmentWriter standardWriter(std::FILE *out);

// The bytes of `in` past its last whole group, written to `out` at the same place:
// what a file in the same format as its input keeps of it besides the samples.
void copyTail(PosFile &in, PosFile &out, SampleFormat fmt);

// A track of samples in a file of any capture format, to a file (any format; the
// bytes past the last whole group are copied when the formats are the same), or
// nowhere. `spec.total` is the samples the file has. Throws rfx::Error.
RequantizeResult requantizeFile(const RequantizeOptions &options, const StreamSpec &spec, PosFile &in, SampleFormat fmt, PosFile *out,
                                SampleFormat outFormat);

// --- Whole jobs ----------------------------------------------------------------

// The job of the tool.
struct RequantizeJob
{
    // The tool does not change formats, and codes again in the codec of the input.
    // The input is a capture (.s16 .r16 .s8 .r8 .lds, or .flac / .ldf through
    // ffmpeg), an .rfx file (either variant), or "-" for signed 16-bit
    // little-endian samples on `standardInput`.
    QString input;
    // Empty: nothing is written, and the report says what a capture would have
    // done. Otherwise what the input is: an .rfx file for an .rfx file, a .flac for
    // a .flac (.ldf for .ldf), a capture file of the input's format for a raw
    // capture, "-" (signed 16-bit samples on `standardOutput`) for a 16-bit raw
    // capture or standard input.
    QString output;
    // The format of `input`: "auto" (from the extension), a raw format, or flac / ldf.
    std::string format = "auto";
    // For a .flac or .ldf output, 0-8 as flac's -0 .. -8.
    int flacCompression = 8;
    // The tracks of an .rfx input to take (0 is the primary), or all.
    std::vector<size_t> only;
    RequantizeOptions options;
    // The samples' rate in MHz. A track of an .rfx file says what it is, a capture
    // may have a .yaml beside it that does; anything else has to be told.
    std::optional<double> rateMhz;
    // A CSV file: one line a segment, what was decided and the slice that held it
    // back. For an .rfx input, with the track as its first column.
    QString logPath;
    // For an .rfx output: the device and threads that code and decode, and whether
    // the coding is checked. The rest of the coding is the input's own.
    PackOptions pack;
    // Where "-" reads and writes, and where the report goes (the output when it is
    // not carrying the samples, else the error stream). Null report: silence.
    std::FILE *standardInput = nullptr;
    std::FILE *standardOutput = nullptr;
    std::FILE *report = nullptr;
    // Progress as a percentage on a single track.
    bool showProgress = false;
};

// Run it. Throws rfx::UsageError for a job that cannot be run as asked, and
// rfx::Error for one that failed.
std::vector<RequantizeResult> runRequantize(const RequantizeJob &job);

} // namespace rfx
