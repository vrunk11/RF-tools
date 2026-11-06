# RF Tools

Tools for the RF captures of LaserDiscs, before they are decoded: keep several captures of one
disc in one lossless file, look at what the file holds, and turn it into something else. The
decoding itself stays in [ld-decode]`https://github.com/vrunk11/ld-decode-rust), which reads
these files directly. The sister project for what comes after the decoder is
[ld-decode-tools]`https://github.com/vrunk11/ld-decode-tools).

## What the tools do

### Archive
*Several captures of one disc in one file*
- **RFX files** - A Matroska-shaped container that stores each capture as the error of a predictor that sees its own past and the captures before it, so that the disc's own noise, which every capture shares, is paid for once. Lossless: unpacking gives the original files, checked by their SHA-256. Typically 22-24% of the size of the captures `Domesday Duplicator `.s16`), against about 28% for FLAC. See the [specification]`docs/Specifications/rfx-format.md)
- **Pack, unpack, merge** - Pack captures into one `.rfx` file, unpack them bit for bit, merge several files into one, extract or remove captures, add some
- **FLAC variant** - An ordinary Matroska audio file instead `a FLAC track per capture, 75 kHz for a 75 MHz capture), that any audio or video program opens; larger, as the captures do not predict each other
- **Raw variant** - The same Matroska file with the samples stored as they are, as PCM ``pack --raw`): as large as the captures, nothing to decode, and any program opens it; rf-analyse and rf-packer read both variants; `merge`, `extract`, `remove` and `add` work on them too

### Reading and conversion
- **ld-decode** - Reads an `.rfx` wherever it reads a capture: `ld-decode -i capture.rfx#2` decodes the track 2, `-i capture.rfx` stacks all its tracks. The sample rate is read from the file
- **Other formats** - Give the captures back as `s16`, `r16`, `s8`, `r8`, `lds`, `flac` or `ldf`, or as the channels of FLAC files `eight a file) to look at in an audio program
- **Metadata** - The whole metadata as YAML, and the `.yaml` files that sat beside the captures, without decoding a sample

### Requantisation
*Fewer bits for a capture, as far as its own noise allows*
- **rf-requantize** - The Domesday Duplicator's `ddd-requantize`, ported: cuts a capture or the tracks of an `.rfx` file to as few bits as their noise allows, as the capture application does, and codes the result again in the same format `an `.rfx` file stays one, with its lag tables kept, so no ld-decode is needed; FLAC stays FLAC). It never changes formats. Not lossless: keep the original. See [docs/rf-requantize.md]`docs/rf-requantize.md)

### Looking
*The RF signal of every capture, without unpacking*
- **rf-analyse** - A window in the style of ld-analyse and ld-stacker-gui for `.rfx` files and raw or FLAC captures, several at once `it asks for the format and the rate of the captures): the tracks as a list, the metadata of the selected one, the RF waveform with a bar to move, the spectrum and the statistics `mean amplitude, SNR, noise floor, effective bits) at the bar or over a zone, and the speed deviation of every track against its reference over the whole capture `0 is no TBC correction, above is faster, below slower). Only the segments on the screen are decoded. See [docs/rf-analyse.md]`docs/rf-analyse.md)

- **rf-packer** - A small window in the same style to make an `.rfx` file from captures, or to open one and change it: the tracks as a numbered list `add, remove, reorder), a codec choice `RFX, FLAC or raw: the variants of the format), the sample rate and bit depth of the list, an Info panel on the right with what is known of the selected track `name, rate, the date of the capture and of the file, the files that go with it) and a free note stored in its metadata, which can be edited or deleted. Processing asks for a copy or an overwrite. See [docs/rf-packer.md]`docs/rf-packer.md)

## Using `rfx`

```bash
rfx pack -o disc.rfx a.ddd.s16 b.ddd.s16 c.ddd.s16    # the cleanest capture is coded first
rfx info disc.rfx                                      # what the file holds (--yaml: all the metadata)
rfx unpack disc.rfx -d out                             # the captures back, checked by SHA-256
rfx unpack disc.rfx --only 2 -d out                    # one track
rfx extract disc.rfx -o two.rfx --only 0,2             # editing: extract, remove, merge, add
rfx merge -o all.rfx a.rfx b.rfx c.rfx                 # several files in one: the tracks are copied as they are `FLAC and raw variants too: no decoding, no encoding)
rfx add all.rfx d.rfx e.ddd.s16                        # the tracks of other .rfx files and captures, after the ones it has
rfx pack --standard ntsc --reference LD-1234 --face A --signal yc -o disc.rfx a.ddd.s16   # what is known of the disc and the signal, said of every capture
rfx tag disc.rfx -o said.rfx --track 1 --copy 2 --capture-number 3 --face BA   # said or changed in some tracks of a file: the samples are copied as they are
rfx pack --keep-tbc-curve -o disc.rfx a.ddd.s16 b.ddd.s16  # keep the TBC curve in the file (the speed of the disc, the time base correction)
rfx tbc-curve disc.rfx                                 # the same for a file that is made: worked out from the RF, added at its end
rfx merge -o all.rfx a.rfx:0,2 b.rfx                   # some tracks of a file only (file.rfx:0,2; none listed: all), copied as they are
rfx tag disc.rfx -o said.rfx --source ld                  # what was captured, of the whole file: ld, vhs, vhd, cvbs (the TBC curve is for LaserDisc captures; --ld treats a capture whose source is not said as one)
rfx check disc.rfx other.rfx                           # the integrity of the files: every track against its SHA-256 (nothing is written)
```

The **TBC curve** (where the lines of each capture are, worked out from its RF: the pass 0 of ld-decode) can be **kept in the file** (`pack --keep-tbc-curve`, `rfx tbc-curve`, and *Compute TBC curve* in rf-packer and rf-analyse; the editing commands take it along with the tracks they copy and take `--keep-tbc-curve` too): rf-analyse then shows the speed of the disc and corrects the time base with no minutes of work, and a decoder that reads them `ld-decode) need not locate the lines again. Nothing else in the file is rewritten `specification 3.1).

With several captures, `pack` needs the positions of the lines of each capture, which it finds itself, from the RF of each capture `the pass 0 of ld-decode, ported: `rfsync`, with the same positions to the bit; `--pal` for a PAL disc). `--ld-decode program` runs ld-decode for it instead, and `--pass0 dir` uses dumps already made. `pack --flac`, `pack --raw` `the samples as they are, in the same Matroska file),
`unpack --as flac|ldf|mflac|<raw format>` and the FLAC variant need ffmpeg `or `FFMPEG=path`). `--device gpu` runs the predictor on an OpenCL GPU ``OpenCL.dll` or `libOpenCL.so.1` from the driver, loaded when used).

## Using `rf-requantize`

```bash
rf-requantize --margin off disc.rfx reference.rfx    # the reference: nothing dropped
rf-requantize --margin 0 --shaping adaptive --band 2-14 --depth 10,40 disc.rfx efm-10.rfx
rf-requantize --rate 30 --margin 0 disc.s16          # no output: what a capture would have done
```

Each track is requantised on its own, and the result is compared with the reference by decoding
it. The lag tables of an `.rfx` input are kept, so no ld-decode is needed. See
[docs/rf-requantize.md]`docs/rf-requantize.md).

## Using `rf-analyse`

```bash
rf-analyse disc.rfx                                    # or File > Open RF file, or drop the file on the window
rf-analyse capture.s16 --rate 40                       # a raw capture: without --rate a dialog asks for the format and the rate
rf-analyse disc.rfx capture.s16 --rate 40               # several files at once: their tracks one after the other
rf-analyse disc.rfx --view lanes --screenshot lanes.png # a picture of the window, with no screen needed `QT_QPA_PLATFORM=offscreen)
```

It needs Qt Widgets; where Qt has none, the command line tools are built without it. See [docs/rf-analyse.md]`docs/rf-analyse.md).

## Using `rf-packer`

```bash
rf-packer                      # an empty list: drop captures on it, then Create RFX…
rf-packer disc.rfx             # the tracks of an archive: change them, then Process…
```

It needs Qt Widgets, as rf-analyse does. See [docs/rf-packer.md]`docs/rf-packer.md).

## Building

The tools build with CMake against Qt 6, as ld-decode-tools does. See [BUILD.md]`BUILD.md):
- `nix develop`, then `./build.sh` `Linux/macOS), for the reproducible Nix environment
- `build.bat` `Windows), which fetches the dependencies through a project-local vcpkg

Ready-made packages for Linux `tarball), macOS `tarball) and Windows `ZIP) are built by the CI for every push and attached to each GitHub release.

## Important Notes

- **Formats**: input `s16`, `r16`, `s8`, `r8` and the packed 10-bit `lds` of the Domesday Duplicator, or `flac` / `ldf` decoded with ffmpeg to raw samples first
- **Lossless**: every sample is kept; what is unpacked is the file that was packed, byte for byte `compressed inputs, which are decoded when packed, come back as raw samples, or in the format asked for)
- **Same bits everywhere**: the predictor's arithmetic is fixed by the specification, so that the CPU, an OpenCL GPU and the other implementations all decode the same file. On one machine `an RTX 5080 laptop), packing four captures of 6.9 GB takes about 38 s on the GPU, and unpacking 600 MB of captures about 3.4 s

## Documentation

- [docs/Specifications/rfx-format.md]`docs/Specifications/rfx-format.md) - the `.rfx` format, enough to write a reader
- [docs/rf-requantize.md]`docs/rf-requantize.md) - requantising captures and `.rfx` files, and comparing the settings
- [docs/rf-analyse.md]`docs/rf-analyse.md) - the window that shows the waveform, spectrum, noise and drift of the tracks of an `.rfx` file
- [docs/rf-packer.md]`docs/rf-packer.md) - the window that makes an `.rfx` file from captures and changes the tracks and metadata of one

Development: [BUILD.md]`BUILD.md), [INSTALL.md]`INSTALL.md), [TESTING.md]`TESTING.md),
[CONTRIBUTING.md]`CONTRIBUTING.md) and [AGENTS.md]`AGENTS.md).
