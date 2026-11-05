# Testing

How RF-tools is tested, locally and in CI.

## Test slices

Every test registered with CTest carries exactly one label. CI runs each label as its own
slice and fails if a test carries neither.

| Label | Tests | Where defined | Needs |
| --- | --- | --- | --- |
| `unit` | `rfx-library-tests`: the range coder and residual model, lag tables, sample formats, the container, the metadata, the YAML numbers, and the predictor against what the reference codes (bit for bit) | `tests/CMakeLists.txt` | the build only |
| `unit` | `rfx-roundtrip-tests`: pack, edit and unpack synthetic captures, and check that the files come back as they went in (also as the FLAC and raw variants, with extract, remove, merge, add, a note and the multichannel export; skipped without ffmpeg) | `tests/CMakeLists.txt` | the build only |
| `unit` | `rf-requantize-tests`: the parsing of bands and depths, the quantiser, the samples the requantiser makes of a fixed capture (hashes taken from the Domesday Duplicator's requantiser), and whole jobs: `.rfx` in and out (lag tables kept, no ld-decode), one track or several, captures, FLAC and the FLAC variant (skipped without ffmpeg), the refusal of a change of format, the report alone | `tests/CMakeLists.txt` | the build only |
| `unit` | `rf-analyse-tests`: the layout of a Duplicator `.yaml` in sections and what it declares, pass 0 on a made-up NTSC signal (the fields, their parity, the line spacing; nothing in a silent capture), the spectrum of a known signal (the bin, the level, the noise floor, the SNR), the speed as the slope of the lag (sign and size), `TrackReader` on raw captures (`.lds`, `.s16`, `.r8`: from any place, the bits found or given) and on the FLAC and raw variants of `.rfx` (every sample exact, in the formats they were packed in; skipped without ffmpeg) and against the captures that were packed: windows inside a segment, across a boundary, over the ends and far away, of a track predicted from another | `tests/CMakeLists.txt` | the build only |
| `unit` | `rf-packer-tests`: the list of tracks (kept, edited, recode, new and removed as the tracks move and go), the job and its JSON, and the window on an offscreen screen: the notes edited and deleted in the Info panel, the name, a copy and an overwrite written by the program run again with `--job`, an archive made from a capture, and the panel that widens the window. Built and run where Qt has Widgets and Qt Test | `tests/CMakeLists.txt` | the build, and `QT_QPA_PLATFORM=offscreen` (set by CTest) |
| `unit` | `rf-analyse-gui-tests`: the session (window, zoom, bar and zone, paging, the results of the worker, a raw capture, the speed measured from the RF on a made-up capture running 1000 ppm fast, the sections the `.yaml` of a capture adds to its metadata), several files at once, the speed deviation, and the table that asks what captures do not say and the window driven with the mouse and the wheel on an offscreen screen: click, drag, wheel, ctrl+wheel, shift+drag, the navigation bar, the track list and its check boxes, the views and a screenshot. Built and run where Qt has Widgets and Qt Test | `tests/CMakeLists.txt` | the build, and `QT_QPA_PLATFORM=offscreen` (set by CTest) |

Add a `functional` label (and its slice in `.github/workflows/build-and-test.yml`) when there
are tests that run the program end to end over fixtures.

### Unit tests

Small executables with in-code inputs. A behaviour change in the library comes with a case in
the matching test. New unit tests:

```cmake
add_test(NAME testfoo COMMAND testfoo)
set_tests_properties(testfoo PROPERTIES LABELS unit)
```

A test that checks with `assert()` must be built with `-UNDEBUG`
(`target_compile_options(testfoo PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)`):
the Release and RelWithDebInfo builds define `NDEBUG`, which removes every `assert()`.

A test that touches the predictor or the coder must give the same bits on every platform: it
decodes a fixed stream to fixed samples, and the expected values are written in the test.

## Running the tests

```bash
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build -L unit --output-on-failure
```

## Continuous integration

`build-and-test.yml` runs, for every push and pull request: the static checks (SPDX headers on
new files, script syntax, flake evaluation), the Nix package build with a smoke test, and the
build with the `unit` slice and a staged install. See [AGENTS.md](AGENTS.md) §7.
