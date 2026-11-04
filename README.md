# RF Tools

Tools for the RF captures of LaserDiscs, before they are decoded: keep several captures of one
disc in one lossless file, look at what the file holds, and turn it into something else. The
decoding itself stays in [ld-decode](https://github.com/vrunk11/ld-decode-rust), which reads
these files directly. The sister project for what comes after the decoder is
[ld-decode-tools](https://github.com/vrunk11/ld-decode-tools).

## What the tools do

### Archive
*Several captures of one disc in one file*
- **RFX files** - A Matroska-shaped container that stores each capture as the error of a predictor that sees its own past and the captures before it, so that the disc's own noise, which every capture shares, is paid for once. Lossless: unpacking gives the original files, checked by their SHA-256. Typically 22-24% of the size of the captures (Domesday Duplicator `.s16`), against about 28% for FLAC. See the [specification](docs/Specifications/rfx-format.md)
- **Pack, unpack, merge** - Pack captures into one `.rfx` file, unpack them bit for bit, merge several files into one, extract or remove captures, add some
- **FLAC variant** - An ordinary Matroska audio file instead (a FLAC track per capture, 75 kHz for a 75 MHz capture), that any audio or video program opens; larger, as the captures do not predict each other

### Reading and conversion
- **ld-decode** - Reads an `.rfx` wherever it reads a capture: `ld-decode -i capture.rfx#2` decodes the track 2, `-i capture.rfx` stacks all its tracks. The sample rate is read from the file
- **Other formats** - Give the captures back as `s16`, `r16`, `s8`, `r8`, `lds`, `flac` or `ldf`, or as the channels of FLAC files (eight a file) to look at in an audio program
- **Metadata** - The whole metadata as YAML, and the `.yaml` files that sat beside the captures, without decoding a sample

### Requantisation
*Fewer bits for a capture, as far as its own noise allows*
- **rf-requantize** - The Domesday Duplicator's `ddd-requantize`, ported: cuts a capture or the tracks of an `.rfx` file to as few bits as their noise allows, as the capture application does, and codes the result again in the same format (an `.rfx` file stays one, with its lag tables kept, so no ld-decode is needed; FLAC stays FLAC). It never changes formats. Not lossless: keep the original. See [docs/rf-requantize.md](docs/rf-requantize.md)

## Using `rfx`

```bash
rfx pack -o disc.rfx a.ddd.s16 b.ddd.s16 c.ddd.s16    # the cleanest capture is coded first
rfx info disc.rfx                                      # what the file holds (--yaml: all the metadata)
rfx unpack disc.rfx -d out                             # the captures back, checked by SHA-256
rfx unpack disc.rfx --only 2 -d out                    # one track
rfx extract disc.rfx -o two.rfx --only 0,2             # editing: extract, remove, merge, add
```

With several captures, `pack` needs the positions of the lines of each capture, which it gets by
running ld-decode's pass 0 (`--ld-decode`, or `--pass0 dir` for dumps already made). `pack --flac`,
`unpack --as flac|ldf|mflac|<raw format>` and the FLAC variant need ffmpeg (or `FFMPEG=path`). `--device gpu` runs the predictor on an OpenCL GPU (`OpenCL.dll` or `libOpenCL.so.1` from the driver, loaded when used).

## Using `rf-requantize`

```bash
rf-requantize --margin off disc.rfx reference.rfx    # the reference: nothing dropped
rf-requantize --margin 0 --shaping adaptive --band 2-14 --depth 10,40 disc.rfx efm-10.rfx
rf-requantize --rate 30 --margin 0 disc.s16          # no output: what a capture would have done
```

Each track is requantised on its own, and the result is compared with the reference by decoding
it. The lag tables of an `.rfx` input are kept, so no ld-decode is needed. See
[docs/rf-requantize.md](docs/rf-requantize.md).

## Building

The tools build with CMake against Qt 6, as ld-decode-tools does. See [BUILD.md](BUILD.md):
- `nix develop`, then `./build.sh` (Linux/macOS), for the reproducible Nix environment
- `build.bat` (Windows), which fetches the dependencies through a project-local vcpkg

Ready-made packages for Linux (tarball), macOS (tarball) and Windows (ZIP) are built by the CI for every push and attached to each GitHub release.

## Important Notes

- **Formats**: input `s16`, `r16`, `s8`, `r8` and the packed 10-bit `lds` of the Domesday Duplicator, or `flac` / `ldf` decoded with ffmpeg to raw samples first
- **Lossless**: every sample is kept; what is unpacked is the file that was packed, byte for byte (compressed inputs, which are decoded when packed, come back as raw samples, or in the format asked for)
- **Same bits everywhere**: the predictor's arithmetic is fixed by the specification, so that the CPU, an OpenCL GPU and the other implementations all decode the same file. On one machine (an RTX 5080 laptop), packing four captures of 6.9 GB takes about 38 s on the GPU, and unpacking 600 MB of captures about 3.4 s

## Documentation

- [docs/Specifications/rfx-format.md](docs/Specifications/rfx-format.md) - the `.rfx` format, enough to write a reader
- [docs/rf-requantize.md](docs/rf-requantize.md) - requantising captures and `.rfx` files, and comparing the settings

Development: [BUILD.md](BUILD.md), [INSTALL.md](INSTALL.md), [TESTING.md](TESTING.md),
[CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md).
