# RF Tools

Tools for the RF captures of LaserDiscs, before they are decoded: keep several captures of one
disc in one lossless file, look at what the file holds, and turn it into something else. The
decoding itself stays in [ld-decode](https://github.com/vrunk11/ld-decode-rust), which reads
these files directly. The sister project for what comes after the decoder is
[ld-decode-tools](https://github.com/vrunk11/ld-decode-tools).

> [!NOTE]
> The tools are written in C++17 with Qt 6, like ld-decode-tools. The command line tool `rfx` is
> being ported from the Rust `rfx` crate of
> [ld-decode-rust](https://github.com/vrunk11/ld-decode-rust) (`crates/rfx`, kept there as
> legacy, with the reader of ld-decode). That crate is the reference: it defines the format, and
> the C++ tools are correct when they give its output.
>
> **Ported so far** (`pack`, `unpack`, `info`, `merge`, `extract`, `remove`, `add`, the FLAC
> variant through ffmpeg, and the OpenCL predictor, `--device gpu`): what they write is the same
> file, byte for byte, as the Rust tool writes from the same captures (on the CPU or on the GPU),
> and what they read is checked by the SHA-256 of the captures. **Not ported**: the development
> commands of the Rust tool (`bench`, `noise`, `refsel`, `lab`, `gpu-check`), and the native FLAC
> reader of ld-decode. There is no graphical application yet.

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

## Building

The tools build with CMake against Qt 6, as ld-decode-tools does. See [BUILD.md](BUILD.md):
- `nix develop`, then `./build.sh` (Linux/macOS), for the reproducible Nix environment
- `build.bat` (Windows), which fetches the dependencies through a project-local vcpkg

Ready-made packages for Linux (tarball), macOS (tarball) and Windows (ZIP) are built by the CI for every push and attached to each GitHub release.

## Important Notes

- **Formats**: input `s16`, `r16`, `s8`, `r8` and the packed 10-bit `lds` of the Domesday Duplicator, or `flac` / `ldf` decoded with ffmpeg to raw samples first
- **Lossless**: every sample is kept; what is unpacked is the file that was packed, byte for byte (compressed inputs, which are decoded when packed, come back as raw samples, or in the format asked for)
- **Same bits everywhere**: the predictor's arithmetic is fixed by the specification, so that the CPU, an OpenCL GPU and the other implementations all decode the same file. The Rust crate packs and unpacks at 30-110 million samples a second on a GPU

## Documentation

- [docs/Specifications/rfx-format.md](docs/Specifications/rfx-format.md) - the `.rfx` format, enough to write a reader

Development: [BUILD.md](BUILD.md), [INSTALL.md](INSTALL.md), [TESTING.md](TESTING.md),
[CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md).
