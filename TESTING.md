# Testing

How RF-tools is tested, locally and in CI.

## Test slices

Every test registered with CTest carries exactly one label. CI runs each label as its own
slice and fails if a test carries neither.

| Label | Tests | Where defined | Needs |
| --- | --- | --- | --- |
| `unit` | `rfx-library-tests`: the range coder and residual model, lag tables, sample formats, the container, the metadata, the YAML numbers, and the predictor against what the reference codes (bit for bit) | `tests/CMakeLists.txt` | the build only |
| `unit` | `rfx-roundtrip-tests`: pack, edit and unpack synthetic captures, and check that the files come back as they went in | `tests/CMakeLists.txt` | the build only |
| `unit` | `rf-requantize-tests`: the parsing of bands and depths, the quantiser, the samples the requantiser makes of a fixed capture (hashes taken from the Domesday Duplicator's requantiser), and whole jobs: `.rfx` in and out (lag tables kept, no ld-decode), one track or several, captures, FLAC and the FLAC variant (skipped without ffmpeg), the refusal of a change of format, the report alone | `tests/CMakeLists.txt` | the build only |

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
