# rf-requantize

Which requantisation settings are right for a player and a disc is a question for experiment:
the margin, the bands, the shaping, its depth and its order all trade file size against what is
left of the noise floor, and the only judge that matters is what ld-decode makes of the result.
Trying each setting during a capture of its own is slow and never quite fair, because no two
plays of a disc are the same.

`rf-requantize` takes one capture, or the tracks of an `.rfx` file, written with
requantisation **off**, and requantises it afterwards, as many times and as many ways as are
worth trying. Every version is made from exactly the same samples, so the only difference
between them is the setting.

It is `ddd-requantize` of the [Domesday Duplicator](https://github.com/simoninns/DomesdayDuplicator)
(Simon Inns), ported, and it reads and writes `.rfx` files.

**It does not change formats**: what comes out is coded as what went in. An `.rfx` file comes
out as an `.rfx` file (of the same variant), a `.flac` as a `.flac`, an `.ldf` as an `.ldf`, a
raw capture as a raw capture of the same format. The samples are read and coded again in the
codec they came in; to change codec, use `rfx unpack --as` and `rfx pack`. **ld-decode is not
needed**; ffmpeg is, for FLAC only (or `FFMPEG=path`).

## Identical to a capture

The requantiser is not a re-implementation: it is the one the capture application drives, in
the same order, from the start of the file, in segments of 2^20 samples, given its settings by
the same translation. A capture requantised here holds exactly the samples the capture
application would have written with those settings, byte for byte. The unit test
(`tests/requantize_tests.cpp`) pins the hash of what the capture application's requantiser makes
of a fixed capture, for a plain and for a shaped setting, so that a change of the decisions is
caught on any machine.

So a setting found here can be taken back to the capture panel, and a capture made with it will
be what was tried.

The format of `.rfx` is not changed, and a requantised `.rfx` file is an ordinary one.

## Usage

```
rf-requantize [options] <input> [<output>]
```

| | |
| --- | --- |
| `<input>` | A capture (`.s16 .r16 .s8 .r8 .lds`, or `.flac` / `.ldf`), an `.rfx` file of either variant, or `-` for signed 16-bit little-endian samples on standard input |
| `<output>` | A file of the same kind: `.rfx` for an `.rfx` file, `.flac` for a `.flac`, `.ldf` for an `.ldf`, the same raw format for a raw capture; or `-` for signed 16-bit samples on standard output (a raw 16-bit capture, or standard input). Without one nothing is written, and the report says what a capture would have done |

| Option | |
| --- | --- |
| `--rate <MHz>` | The samples' rate. A track of an `.rfx` file says what it is, and so can a `.yaml` beside a capture; anything else (a `.flac` included: its header holds the rate in kHz) has to be told: `30` for 60 MHz halved. Given, it wins over what the file says, and is the rate written to an `.rfx` output |
| `--margin <0-4\|off>` | How far the noise floor may rise in any 1 MHz slice of the protected bands: `0` aggressive (1 dB), `1` (0.5 dB), `2` safe (0.2 dB, the default), `3` (0.1 dB), `4` (0.05 dB). `off` writes the samples as they are: the reference the others are compared with |
| `--band <bands>` | The protected bands, in MHz: `0-12`, `2-14`, or several, `0-1.9,2.1-13.5`. Default: DC to 14 MHz, or 1.5 MHz short of the Nyquist limit |
| `--shaping <shaping>` | `fixed` (the default) or `adaptive` |
| `--depth <dB>[,<dB>...]` | How far above the bands the shaped noise may go, 0 to 40. One depth for everywhere outside the bands, or one for each stretch between them from DC up: with bands `2-14`, `10,40` spares the EFM below 2 MHz and puts the rest above 14 MHz. Fewer depths than stretches repeat the last; more is an error. Default 10 fixed, 20 adaptive |
| `--order <2-64>` | The shaping filter's order. Default 16 fixed, 32 adaptive |
| `--input-bits <5-16>` | The bits the input carries. A track of an `.rfx` file says, through its `shift`; otherwise they are found from the first segment |
| `--format <format>` | The format of the input capture when its extension does not say: `s16`, `r16`, `s8`, `r8`, `lds`, `flac`, `ldf` |
| `--only <tracks>` | The tracks of an `.rfx` input to take (0 is the primary), e.g. `0,3,5` |
| `--compression <0-8>` | The FLAC level of a `.flac` or `.ldf` output, as flac's `-0` to `-8`. Default 8 |
| `--log <file.csv>` | One line per segment: the track, what was decided, the noise floor it was decided against, and the slice of the bands that held it back |
| `--realtime` | Hand the samples on no faster than their rate, as a capture does. See [Piping](#piping) |
| `--no-verify`, `--device`, `--threads` | For an `.rfx` file: skip the check of each coded segment, run the predictor on `cpu` or an OpenCL `gpu`, and the number of threads |

| Exit code | |
| --- | --- |
| 0 | Done |
| 1 | The work failed: a file that cannot be read or written, data that cannot be used |
| 2 | The command line was wrong, including a change of format and settings that cannot be used together |

## With `.rfx` files

An `.rfx` input is decoded track by track (each checked against its SHA-256, as `rfx unpack`
does) and **each track is requantised on its own**: the requantiser decides from the noise of
that capture, so two captures of one disc may lose a different number of bits. `--only` takes
some of the tracks.

The output is coded again as the input was. The FLAC and the raw variants of `.rfx` stay what they were
(their tracks are not predicted from each other, so it has no lag tables to keep, and needs no
ld-decode either). For the predictor variant: Requantising changes the value of the samples, not
their place, so the lines are where they were: the **lag tables are kept, not worked out
again**, with the references, the weights, the predictor parameters, the segment length, the
`.yaml` files, the sample rates, the notes, what was said of the disc and the TBC curve of each track (the curve belongs to the track and the lines do not move, so it stays with it, as long as the track is kept). No pass 0 is run and ld-decode is not needed.
With `--only`, a track keeps the references that are among the tracks taken. The bits that were
dropped are zero bits, which the `shift` of the format makes free, so the file is much smaller
than the original.

The `.rfx` file is lossless; **requantisation is not**: the output is not the capture it was
made from, and unpacking it gives the requantised samples. Keep the original.

Work files (the decoded tracks) are made in a directory beside the output, or in the system's
temporary directory when nothing is written, and removed at the end. They take as much room as
the captures.

## A comparison, step by step

1. **Capture once with requantisation off**, at the rate you mean to use, and keep it as it is,
   or packed with `rfx pack`.

2. **Write the reference** through the same coding, with nothing dropped. Comparing against the
   original file would also measure the difference between two encodes.

   ```bash
   rf-requantize --margin off disc.rfx reference.rfx
   ```

3. **Write each setting worth trying.**

   ```bash
   rf-requantize --margin 0 --shaping adaptive --band 2-14 --depth 40 disc.rfx depth-40.rfx
   rf-requantize --margin 0 --shaping adaptive --band 2-14 --depth 10,40 disc.rfx efm-10.rfx
   ```

4. **Compare.** Each run ends with the bits dropped and for how much of the capture, how much of
   it was shaped, the worst rise in the protected bands and the size written. Then decode each
   file with ld-decode (`ld-decode -i efm-10.rfx`) and compare the picture, the VITS figures
   ld-analyse gives for a disc that has them, and the EFM error counts, against the reference.

The setting worth keeping is the smallest file whose picture and EFM cannot be told from the
reference's.

A capture is decided segment by segment from its start, so a short extract of a long capture is
not decided exactly as that stretch was within the whole: the floor the first segments are
measured against is the extract's own. Compare whole captures, or extracts against extracts.

## Piping

`-` reads signed 16-bit samples from standard input and writes them to standard output, so the
tool fits between two programs. Standard input has no rate to say, so `--rate` is needed there.
When standard output carries the samples, the report goes to standard error, and nothing but
samples reaches the pipe.

```bash
rf-requantize --rate 30 --margin 0 --shaping adaptive --band 2-14 --depth 10,40 disc.s16 - | decoder
```

A file is read as fast as it can be, and a pipe normally holds the writer back to the reader's
pace. A program downstream that expects a live stream, paced at the sample rate, is given one
with `--realtime`: each segment is handed on no sooner than the moment its last sample would have
been taken. The report says if the run fell behind real time.

On Windows, pipe from `cmd.exe`, Git Bash or PowerShell 7.4 and later. Windows PowerShell 5.1
re-encodes what passes between two programs as text, which corrupts samples.

## What it writes

A raw capture is the samples alone, in the input's format, with the bytes past its last whole
sample kept. A `.flac` or `.ldf` output (through ffmpeg) carries the four `DDD_REQUANTIZATION`
tags that describe what was done, as the capture application writes them. Nothing else is
written: no metadata sidecar, and the CSV log only when it is asked for. The report goes to standard output, or to standard error when standard output is carrying
the samples.
