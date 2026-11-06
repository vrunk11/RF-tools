# RF Tools

Tools for the RF captures of LaserDiscs, before they are decoded: keep several captures of one
disc in one lossless file, look at what the file holds, and turn it into something else. The
decoding itself stays in [ld-decode](https://github.com/vrunk11/ld-decode-rust), which reads
these files directly. The sister project for what comes after the decoder is
[ld-decode-tools](https://github.com/vrunk11/ld-decode-tools).

<img width="1703" height="698" alt="{EF3F9849-9170-4107-B727-67D98B079770}" src="https://github.com/user-attachments/assets/4caa6983-a17c-4570-97c2-83492abeaa80" />

## What the tools do

| Tool | What it is |
| --- | --- |
| `rfx` | The command line: pack, unpack, merge, extract, remove, add, tag, check, TBC curve |
| `rf-packer` | A window to make an `.rfx` file from captures, or to open one and change it |
| `rf-analyse` | A window to look at the RF signal of every capture, without unpacking |
| `rf-requantize` | Fewer bits for a capture, as far as its own noise allows |

### Archive
*Several captures of one disc in one file*
- **RFX files** - A Matroska-shaped container that stores each capture as the error of a predictor that sees its own past and the captures before it, so that the disc's own noise, which every capture shares, is paid for once. Lossless: unpacking gives the original files, checked by their SHA-256. Typically 22-24% of the size of the captures (Domesday Duplicator `.s16`), against about 28% for FLAC. See the [specification](docs/Specifications/rfx-format.md)
- **Pack, unpack, merge** - Pack captures into one `.rfx` file, unpack them bit for bit, merge several files into one **without coding anything again**, extract or remove captures, add some. The tracks you want can be picked from each file (`a.rfx:0,2`)
- **Check** - Verify every track of a file against its SHA-256, in the windows and on the command line
- **FLAC variant** - An ordinary Matroska audio file instead (a FLAC track per capture, 75 kHz for a 75 MHz capture), that any audio or video program opens; larger, as the captures do not predict each other
- **Raw variant** - The same Matroska file with the samples stored as they are, as PCM (`pack --raw`): as large as the captures, nothing to decode, and any program opens it. rf-analyse and rf-packer read both variants; `merge`, `extract`, `remove` and `add` work on them too, by copying the streams

### What is known of the disc
*Said by a person, kept in the file, never guessed from the samples*
- **Source of the file** - What was captured, for the whole file: **LD** (a LaserDisc), **VHS**, **VHD**, **CVBS** or **unknown** (the default). The work that is a LaserDisc's (the TBC curve and what is made from it) is done for LD captures: for an unknown source the tools ask whether to read the capture as a LaserDisc, and for another source they say why they do not do it
- **Per track** - The video standard (PAL, NTSC, PAL-M, SECAM, NTSC-VHD), a reference of the disc, the number of the physical copy, the number of the capture, the face (A, B, AB, BA) and whether the signal is composite (CVBS) or Y/C, plus a free note
- **Everywhere** - Said and read in rf-packer, rf-analyse and `rfx` (`pack`, `tag`)

### The TBC curve
*Where the lines of each capture are, worked out from its RF (the pass 0 of ld-decode)*
- **Kept in the file** - Worked out once, added at the end of the `.rfx` file (nothing else is rewritten): the speed of the disc and the time base correction are then there at once for rf-analyse and for ld-decode, which need not locate the lines again. The editing commands carry it along with the tracks they copy
- **A quick look first** - Before the long work, the start of every track is looked at to tell its video system (PAL or NTSC). If the standard said for a track seems wrong, the tools list the tracks and offer to correct it
- **Same positions as ld-decode** - A port of the pass 0 of ld-decode-rust (`rfsync`), with the same positions to the bit

### Reading and conversion
- **ld-decode** - Reads an `.rfx` wherever it reads a capture: `ld-decode -i capture.rfx#2` decodes the track 2, `-i capture.rfx` stacks all its tracks. The sample rate is read from the file
- **Other formats** - Give the captures back as `s16`, `r16`, `s8`, `r8`, `lds`, `flac` or `ldf`, or as the channels of FLAC files (eight a file) to look at in an audio program
- **Metadata** - The whole metadata as YAML, and the `.yaml` files that sat beside the captures, without decoding a sample

### Requantisation
*Fewer bits for a capture, as far as its own noise allows*
- **rf-requantize** - The Domesday Duplicator's `ddd-requantize`, ported: cuts a capture or the tracks of an `.rfx` file to as few bits as their noise allows, as the capture application does, and codes the result again in the same format (an `.rfx` file stays one, with its lag tables, its metadata and its TBC curve kept, so no ld-decode is needed; FLAC stays FLAC). It never changes formats. Not lossless: keep the original. See [docs/rf-requantize.md](docs/rf-requantize.md)

### Looking
*The RF signal of every capture, without unpacking*

- **rf-analyse** - A window in the style of ld-analyse and ld-stacker-gui for `.rfx` files and raw or FLAC captures, several at once (it asks for the format and the rate of the captures). Only the segments on the screen are decoded.
  - the tracks as a list (with a progress bar for the work in hand), the metadata of the selected one, and its checksum
  - the RF waveform with a bar to move, the spectrum, and the statistics (mean amplitude, SNR, noise floor, effective bits) at the bar or over a zone
  - the speed deviation of every track over the whole capture (0 is no TBC correction, above is faster, below slower), measured from the RF itself
  - **Align capture**: the time base of every track is corrected, so that the captures lie on one axis; **Stack** shows the mean of the tracks, and **Noise residue** what each track has more than the stack
  - **Check** (the integrity of a track or of the file), **Compute TBC curve**, **Save TBC curve**, and **Edit…** (name, note, what is known of the disc, and the source of the file; the file is written again, its samples copied as they are, without closing it)

  See [docs/rf-analyse.md](docs/rf-analyse.md)

#### Combined view :
<img width="1189" height="604" alt="{40DBF8D7-6885-4D6A-9F51-0FEBA10C77FE}" src="https://github.com/user-attachments/assets/17cb0e23-bfb8-465d-93ee-826c5789c8f4" />

<img width="1178" height="604" alt="{13F0308F-700E-4206-A572-3682CE81A25C}" src="https://github.com/user-attachments/assets/9b1f7f45-a0dd-4bc0-bd77-0272ee051fdf" />

<img width="912" height="691" alt="{1B1F6CA7-B345-43FA-9AE7-3C24DABC7569}" src="https://github.com/user-attachments/assets/d1cf6298-c60a-4890-bde6-018e70606922" />

<img width="1463" height="738" alt="{71094C33-32C8-4181-8D87-492A1EA1230A}" src="https://github.com/user-attachments/assets/62b4332c-4333-4779-8d5d-0a1731c456d9" />


- **rf-packer** - A small window in the same style to make an `.rfx` file from captures, or to open one (or several) and change it.
  - the tracks as a numbered list (add, remove, reorder), a codec choice (RFX, FLAC or raw: the variants of the format), the sample rate and bit depth of the list, and the **Source** of the whole file
  - an Info panel with what is known of the selected track (name, rate, the date of the capture and of the file, the files that go with it, the standard, the reference, the copy, the capture number, the face, the signal) and a free note
  - several `.rfx` files can be opened together and written as one, **without coding again** the tracks that can be copied as they are
  - **Compute TBC curve** for the file or for one track, **Check**, **Unpack** all or one track, with a real progress; the work runs on the GPU by default when there is one
  - the tracks whose standard seems wrong are listed before the work of the TBC curve, to be corrected

  See [docs/rf-packer.md](docs/rf-packer.md)

<img width="1311" height="969" alt="{4475D8FB-DA61-4B5B-9154-62B41CDC35A5}" src="https://github.com/user-attachments/assets/fe47a923-f50f-4b00-97d4-12dc2828b606" />

## Using `rfx`

```bash
rfx pack -o disc.rfx a.ddd.s16 b.ddd.s16 c.ddd.s16    # the cleanest capture is coded first
rfx info disc.rfx                                      # what the file holds (--yaml: all the metadata)
rfx check disc.rfx other.rfx                           # the integrity of the files: every track against its SHA-256 (nothing is written)
rfx unpack disc.rfx -d out                             # the captures back, checked by SHA-256
rfx unpack disc.rfx --only 2 -d out                    # one track
rfx extract disc.rfx -o two.rfx --only 0,2             # editing: extract, remove, merge, add
rfx merge -o all.rfx a.rfx b.rfx c.rfx                 # several files in one: the tracks are copied as they are (FLAC and raw variants too: no decoding, no encoding)
rfx merge -o all.rfx a.rfx:0,2 b.rfx                   # some tracks of a file only (file.rfx:0,2; none listed: all)
rfx add all.rfx d.rfx e.ddd.s16                        # the tracks of other .rfx files and captures, after the ones it has
```

What is known of the disc, and the TBC curve:

```bash
rfx pack --source ld --standard ntsc --reference LD-1234 --face A --signal yc -o disc.rfx a.ddd.s16   # said of the file and of every capture
rfx tag disc.rfx -o said.rfx --track 1 --copy 2 --capture-number 3 --face BA   # said or changed in some tracks of a file: the samples are copied as they are
rfx tag disc.rfx -o said.rfx --source ld               # what was captured, of the whole file: ld, vhs, vhd, cvbs or unknown
rfx pack --source ld --keep-tbc-curve -o disc.rfx a.ddd.s16 b.ddd.s16   # keep the TBC curve in the file (the speed of the disc, the time base correction)
rfx tbc-curve disc.rfx                                 # the same for a file that is made: worked out from the RF, added at its end
rfx tbc-curve --ld disc.rfx                            # for a file whose source is not said: treat it as a LaserDisc capture
```

The **TBC curve** is worked out for LaserDisc captures: with `--source ld` it is, for a source that is not said it is only if you say `--ld` (the command line cannot ask), and for another source (VHS, VHD, CVBS) it is not. rf-analyse then shows the speed of the disc and corrects the time base with no minutes of work, and a decoder that reads them (ld-decode) need not locate the lines again. Nothing else in the file is rewritten (specification section 3.1).

With several captures, `pack` needs the positions of the lines of each capture, which it finds itself, from the RF of each capture (the pass 0 of ld-decode, ported: `rfsync`, with the same positions to the bit; `--pal` for a PAL disc, which `--standard pal` says too). `--ld-decode program` runs ld-decode for it instead, and `--pass0 dir` uses dumps already made. `pack --flac`, `pack --raw` (the samples as they are, in the same Matroska file), `unpack --as flac|ldf|mflac|<raw format>` and the FLAC variant need ffmpeg (or `FFMPEG=path`). `--device gpu` runs the predictor on an OpenCL GPU (`OpenCL.dll` or `libOpenCL.so.1` from the driver, loaded when used).

## Using `rf-requantize`

```bash
rf-requantize --margin off disc.rfx reference.rfx    # the reference: nothing dropped
rf-requantize --margin 0 --shaping adaptive --band 2-14 --depth 10,40 disc.rfx efm-10.rfx
rf-requantize --rate 30 --margin 0 disc.s16          # no output: what a capture would have done
```

Each track is requantised on its own, and the result is compared with the reference by decoding
it. The lag tables of an `.rfx` input are kept, so no ld-decode is needed; what was said of the disc, the source of the file and the TBC curve stay with the tracks. See
[docs/rf-requantize.md](docs/rf-requantize.md).

## Using `rf-analyse`

```bash
rf-analyse disc.rfx                                    # or File > Open RF file, or drop the file on the window
rf-analyse capture.s16 --rate 40                       # a raw capture: without --rate a dialog asks for the format and the rate
rf-analyse disc.rfx capture.s16 --rate 40              # several files at once: their tracks one after the other
rf-analyse disc.rfx --align                            # start with the time base of every track corrected
rf-analyse disc.rfx --ld                               # treat the captures whose source is not said as LaserDisc captures, without asking
rf-analyse disc.rfx --view lanes --screenshot lanes.png # a picture of the window, with no screen needed (QT_QPA_PLATFORM=offscreen)
```

It needs Qt Widgets; where Qt has none, the command line tools are built without it. The window remembers the last folder it opened. See [docs/rf-analyse.md](docs/rf-analyse.md).

## Using `rf-packer`

```bash
rf-packer                      # an empty list: drop captures on it, then Create RFX…
rf-packer disc.rfx             # the tracks of an archive: change them, then Process…
```

It needs Qt Widgets, as rf-analyse does. See [docs/rf-packer.md](docs/rf-packer.md).

## Building

The tools build with CMake against Qt 6, as ld-decode-tools does. See [BUILD.md](BUILD.md):
- `nix develop`, then `./build.sh` (Linux/macOS), for the reproducible Nix environment
- `build.bat` (Windows), which fetches the dependencies through a project-local vcpkg

Ready-made packages for Linux (tarball), macOS (tarball) and Windows (ZIP) are built by the CI for every push and attached to each GitHub release.

## Important Notes

- **Formats**: input `s16`, `r16`, `s8`, `r8` and the packed 10-bit `lds` of the Domesday Duplicator, or `flac` / `ldf` decoded with ffmpeg to raw samples first
- **Lossless**: every sample is kept; what is unpacked is the file that was packed, byte for byte (compressed inputs, which are decoded when packed, come back as raw samples, or in the format asked for)
- **Same bits everywhere**: the predictor's arithmetic is fixed by the specification, so that the CPU, an OpenCL GPU and the other implementations all decode the same file. On one machine (an RTX 5080 laptop), packing four captures of 6.9 GB takes about 38 s on the GPU, and unpacking 600 MB of captures about 3.4 s
- **LaserDisc captures**: the TBC curve, the alignment and the stack are for LaserDisc captures. A file of another source is refused with a message, and one that does not say asks first

## Documentation

- [docs/Specifications/rfx-format.md](docs/Specifications/rfx-format.md) - the `.rfx` format, enough to write a reader
- [docs/rf-requantize.md](docs/rf-requantize.md) - requantising captures and `.rfx` files, and comparing the settings
- [docs/rf-analyse.md](docs/rf-analyse.md) - the window that shows the waveform, spectrum, noise and drift of the tracks of an `.rfx` file
- [docs/rf-packer.md](docs/rf-packer.md) - the window that makes an `.rfx` file from captures and changes the tracks and metadata of one

Development: [BUILD.md](BUILD.md), [INSTALL.md](INSTALL.md), [TESTING.md](TESTING.md),
[CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md).
