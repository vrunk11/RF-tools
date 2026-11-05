# AGENTS.md

Conventions for anyone — human or automated assistant — working in this repository.

Everything here applies to agents *and* to people. Where a rule exists specifically because
assistants get it wrong by default, that is called out.

---

## Rule 1 — No automatic git operations

**Never run a command that changes repository state unless the user has explicitly asked for
that specific action in that specific message.**

> **Forbidden without an explicit request:** `git add`, `git commit`, `git push`,
> `git stash`, `git rebase`, `git reset`, `git revert`, `git merge`, `git cherry-pick`,
> `git tag`, `git branch -d`/`-D`, any `git checkout`/`git switch` that discards changes,
> `git clean`, `git submodule update`, and any `gh pr create` / `gh pr merge`.
>
> **Always permitted:** `git status`, `git log`, `git diff`, `git show`, `git blame`,
> `git ls-files`, `git grep` — anything read-only.

- **"Make this change" means edit the files.** It does not imply staging or committing.
- **Permission for one commit does not carry to the next.** Each one is asked for separately.
- **Finishing a task is not a trigger to commit.** Leave the work in the working tree and
  report what changed.
- A dirty working tree at the end of a task is the expected outcome, not a loose end.

## Rule 2 — No AI attribution anywhere

**Nothing this project produces may advertise the tools used to produce it.**

> **Forbidden** in commit messages, PR titles and bodies, code comments, documentation and
> release notes:
>
> - `Co-Authored-By:` naming an AI tool or service
> - `Generated with …`, `Created by …`, `Written with the help of …`
> - `🤖`, "AI-assisted", or any tool or vendor name used as an attribution

Commit messages describe **the change**, not how it was written. Several assistants append
attribution trailers by default unless told not to — this rule is that instruction.

The same restriction covers advertisements, promotions and commercial references generally.

---

## 1. What this project is

**RF-tools** is the set of tools that work on the RF captures of LaserDiscs before they are
decoded: archiving several captures of one disc in one lossless file (`.rfx`), looking at what
such a file holds, and converting it. Decoding stays in
[ld-decode](https://github.com/vrunk11/ld-decode-rust), which reads `.rfx` files directly.

The tools are written in **C++17 with Qt 6**, in the style of
[ld-decode-tools](https://github.com/vrunk11/ld-decode-tools); this repository holds only that
C++ code and the specifications. The **reference implementation is the Rust `rfx` crate of
[ld-decode-rust](https://github.com/vrunk11/ld-decode-rust)**: it defines the format and every
bit of the predictor, and a C++ tool is correct when it gives that crate's output. Its writer
(and the experiments) is kept on the branch `legacy/rfx-packer` of that repository; on
`vrunk11_main` the crate (`crates/rfx`) is the reader that ld-decode uses.

- **Type:** C++17, Qt 6
- **Build system:** CMake 3.16+, Ninja
- **Licence:** GPL-3.0-or-later

### 1.1 Tools

| Directory | Binaries | Purpose |
| --- | --- | --- |
| `src/rfx/` | `rfx` | The command line tool: `pack`, `unpack`, `info`, `merge`, `extract`, `remove`, `add`, and the diagnostics `noise`, `refsel`, `bench`, `gpu-check`, and `pass0` (the lines of captures located from their RF, in the dump form of ld-decode) (ported from the Rust `rfx`, except its `lab` experiments; the FLAC and raw variants go through ffmpeg, and every command reads and writes them) |
| `src/requantize/` | `rf-requantize` | Requantising a capture or the tracks of an `.rfx` file as the Domesday Duplicator's capture application does (ported from its `ddd-requantize`); reads and writes `.rfx` files and captures, always in the format of the input (FLAC through ffmpeg; no ld-decode, the lag tables are kept). See `docs/rf-requantize.md` |
| `src/analyse/` | `rf-analyse` | The window (Qt Widgets) that shows the RF signal of the tracks of an `.rfx` file or of a raw or FLAC capture (a table asks what captures do not say; several files open together): tracks, metadata, waveform, spectrum, statistics, speed deviation. Needs Qt Widgets, and is not built where Qt has none. See `docs/rf-analyse.md` |
| `src/packer/` | `rf-packer` | The window (Qt Widgets) that makes an `.rfx` file from captures or changes the tracks of one: the numbered list, the Info panel (editable name, rate, sidecar files and notes; the capture and file dates) and the writing, which runs in the program itself started again with `--job`. Needs Qt Widgets. See `docs/rf-packer.md` |
| `src/gui-common/` | (static library `rfx-gui-common`) | What the windows share: the colour themes and widget styles with their View menus, the theme editor, the collapse arrow and the number formats |
| `src/library/` | (static library `rfx-library`) | The format: container, metadata, predictor (CPU, and OpenCL in `kernel.cl`, loaded at run time), range coder, lag tables, reference windows, packing, editing, unpacking, conversion; the requantiser (`requantizer.*`, the streams and files it works on in `requantize.*`, whole jobs in `requantizejob.cpp`), and reading at any place (`trackreader.*` decodes only the segments asked for, or the frames of the FLAC and raw variants that hold the samples, `analysis.*` makes spectra, statistics and lag series), and the pass 0 (`rfsync.*`: the lines and frame codes of a capture, found from its RF; a port of ld-decode-rust's `rfsync`, giving the same positions to the bit). Shared by every tool |
| `tests/` | `rfx-library-tests`, `rfx-roundtrip-tests`, `rf-requantize-tests`, `rf-analyse-tests`, `rf-analyse-gui-tests`, `rf-packer-tests` | The unit tests (CTest label `unit`) |

### 1.2 Repository layout

```
├── AGENTS.md                 # this file (CLAUDE.md and .github/copilot-instructions.md point here)
├── TESTING.md                # test slices and how to run them
├── CONTRIBUTING.md
├── BUILD.md / INSTALL.md     # Nix build and install
├── CMakeLists.txt            # top level: options, Qt, the tools
├── build.sh / build.bat      # local builds (Windows: project-local vcpkg)
├── flake.nix                 # the package and the dev shell
├── vcpkg.json                # Windows dependencies (and Qt, under the `qt` feature)
├── src/                      # one directory per tool
├── tools/                    # repository scripts run by CI (check-spdx-new-files.sh)
├── docs/
│   └── Specifications/       # the file formats, kept true (§2)
│       └── rfx-format.md
└── .github/workflows/        # CI/CD, see §7
```

## 2. File formats

- The formats that belong to this project (`.rfx`, in both of its variants) are specified in
  `docs/Specifications/`, and the specification is kept true: **a change to how one of them is
  read, written or behaves (a field, a parameter, a default, the arithmetic of the predictor)
  updates the specification in the same change**, with the tools' pages in `docs/` that describe
  it. Behaviour that is not in the specification yet is added to it.
- The predictor's arithmetic is part of the format: the same `f32` operations in the same order,
  no fused multiply-add, so that every implementation (the CPU one, the OpenCL one, the C++ one,
  the one in ld-decode) gives the same bits. A change there is a format change.
- While the format is at version 1 and in development, it may change; once a version is
  released, a change is a new version. Call any change of the on-disk format out in the PR and
  ask before making it (§10).
- The reader in ld-decode (`crates/rfx` of ld-decode-rust) must keep reading what the tools
  write. A format change updates both.

## 3. Do not hand-edit generated or golden files

| Path | Rule |
| --- | --- |
| `test-data/**` | Golden fixtures (to come). Do not regenerate or modify them to make a test pass. A change that legitimately alters output updates the **expected values**, with the reason in the PR |

## 4. Coding standards

### 4.1 C++

- C++17 (`CMAKE_CXX_STANDARD 17`), Qt 6. Use Qt types and idioms (`QString`, `QVector`,
  `QCommandLineParser`).
- **There is no formatter gate in this repository.** Match the style of the file you are
  editing, and **do not reformat code you are not otherwise changing** — whitespace-only diffs
  bury the change.
- Keep tools independent: shared logic goes in `src/library/`, never in another tool's
  directory.
- Code must stay portable across Linux (GCC), macOS (Clang) and Windows (MSVC). No POSIX-only
  headers (`unistd.h`, `getopt`) in tool code; use Qt equivalents.
- The predictor must give the same bits on every platform: no `-ffast-math`, no
  contraction of multiply-add (`-ffp-contract=off`), no reassociation, `float` arithmetic
  as the specification gives it.
- Avoid magic numbers; name constants and cite the specification section where one applies.

### 4.2 Licence headers

The project is **GPL-3.0-or-later**. New source files (`.cpp .h .hpp .c .py .sh`) **must**
carry an SPDX header:

```cpp
/******************************************************************************
 * filename.cpp
 * tool-name - one-line purpose
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Contributor Name
 *
 * This file is part of RF-tools.
 ******************************************************************************/
```

Existing files without SPDX are converted only when already being edited, never in a sweep.

### 4.3 Naming

- Never name a file, symbol, branch or CMake target after a plan step or iteration
  (`phase0`, `v2`, `new`, `final`, `refactored`). Name things after what they do.

### 4.4 Exit codes of the command line tools

A tool exits with **0** when it did what it was asked, **2** when the command line is wrong (an
option or a value that is not valid, an argument missing or too many, options that cannot go
together), and **1** when the work failed (a file that cannot be opened or written, data that
cannot be used, a stage that stops). Not -1, which a shell sees as 255.

## 5. Testing

Source of truth: [TESTING.md](TESTING.md). In short:

| CTest label | What | Needs |
| --- | --- | --- |
| `unit` | the tests of the library and of the windows (the windows are tested with no screen) | nothing beyond the build |

- **Every** `add_test()` must carry exactly one label; CI fails on an unlabelled test. Add a
  `functional` label, and its slice in `build-and-test.yml`, when tests run the program over
  fixtures.
- A lossless tool is tested by the round trip: pack, unpack, and the hash of the result against
  the hash of the input (`sha256` in the metadata).
- A change to the predictor, the coder or the container needs a test that decodes a fixed
  stream to fixed samples, so that a regression of the bits is caught on any machine.
- The reference is the Rust `rfx` crate of ld-decode-rust: its writer is on the branch `legacy/rfx-packer` (build it there, `cargo build --release -p rfx`), its reader on `vrunk11_main` (`cargo test --release -p rfx`). A
  C++ tool is checked against its output on the same captures, bit for bit.
- Do not loosen an expected value to make a test pass without explaining why the new value is
  correct.

## 6. Development environment

**Nix is the supported environment**; CI's Linux gate runs inside the same dev shell.

```bash
nix develop                                   # all dependencies (Qt6, ffmpeg, ccache)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build -L unit --output-on-failure
nix build .#                                  # the package, as Nix users get it
```

One-off without entering the shell: `nix develop -c <command>`. For a temporary tool, use
`nix shell nixpkgs#<tool> -c <tool> ...` — do **not** add it to `flake.nix`.

- `./build.sh` (Linux/macOS) and `build.bat` (Windows) build into `build/`, tuned for the
  machine (`NATIVE=0` for a portable build); see [BUILD.md](BUILD.md).
- In-source builds are rejected; use `build/` or `build-*/`.
- Windows: `build.bat` uses a project-local vcpkg (`vcpkg/`, manifest `vcpkg.json`, Qt under
  the `qt` feature). Keep `vcpkg.json` and `package-windows.yml` in step.

## 7. CI/CD

All in `.github/workflows/`, entered through `main.yml` (the same as ld-decode-tools):

| Workflow | When | What |
| --- | --- | --- |
| `main.yml` | push (all branches, `v*` tags), PR, dispatch | Orchestrates everything below |
| `build-and-test.yml` | always | Static checks (SPDX on new files, script syntax, flake eval); `nix build` with a smoke test; dev-shell build + `unit` CTest slice + staged install |
| `package-linux.yml` | push / dispatch | Qt 6.9 build → linuxdeploy → tarball |
| `package-macos.yml` | push / dispatch | Qt 6.9 build → QtCore framework beside the program → tarball |
| `package-windows.yml` | push / dispatch | MSVC + Qt 6.9 → `windeployqt` → ZIP |
| `release-from-artifact.yml` | `v*` tags | One GitHub release with every platform's package |

The smoke tests run `rfx --version` on the packaged program.

Before proposing build-system or dependency changes, read the platform workflow they affect
and say in the PR what was validated locally and what only CI can show (e.g. Windows/MSVC,
macOS, linuxdeploy). If documentation and a working workflow disagree, the workflow is right
— flag the mismatch.

## 8. Licensing

GPL-3.0-or-later. New dependencies must be GPLv3-compatible (GPL, LGPL, BSD, MIT, Apache-2.0,
ISC, …; not AGPL-only, SSPL or proprietary). State the licence of any dependency you add.

## 9. Contribution hygiene

- One problem per PR; no unrelated refactors.
- Update the tool's `README.md` / `docs/` when user-visible behaviour or options change.
- Never hard-code secrets or tokens; validate external input (paths, command-line arguments,
  archive metadata: an archive is data from outside).

## 10. Ask first when

- a change affects the file format, the metadata schema, command-line options or output
  compatibility;
- several valid approaches have materially different trade-offs;
- a change has licensing, security or performance implications.

## 11. Communication style

Be concise. Prefer bullet lists and fenced commands. Cite files and lines
(`src/library/rfx/predictor.cpp:120`) when referring to code.
