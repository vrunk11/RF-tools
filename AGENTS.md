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
C++ code and the specifications. The **reference implementation is the Rust `rfx` crate in
[ld-decode-rust](https://github.com/vrunk11/ld-decode-rust)** (`crates/rfx`, kept there as
legacy, with the reader that ld-decode uses): it defines the format and every bit of the
predictor, and a C++ tool is correct when it gives that crate's output.

- **Type:** C++17, Qt 6
- **Build system:** CMake 3.16+, Ninja
- **Licence:** GPL-3.0-or-later

### 1.1 Tools

| Directory | Binaries | Purpose |
| --- | --- | --- |
| `src/` | (none yet) | The C++/Qt tools, one directory per tool, shared code in `src/library/` |

### 1.2 Repository layout

```
├── AGENTS.md                 # this file (CLAUDE.md and .github/copilot-instructions.md point here)
├── CONTRIBUTING.md
├── README.md
├── LICENSE                   # GPL-3.0-or-later
├── docs/
│   └── Specifications/       # the file formats, kept true (§2)
│       └── rfx-format.md
└── src/                      # the C++/Qt tools (to come)
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

- A lossless tool is tested by the round trip: pack, unpack, and the hash of the result against
  the hash of the input (`sha256` in the metadata).
- A change to the predictor, the coder or the container needs a test that decodes a fixed
  stream to fixed samples, so that a regression of the bits is caught on any machine.
- The reference is the Rust `rfx` crate of ld-decode-rust (`cargo test --release -p rfx` there): a
  C++ tool is checked against its output on the same captures, bit for bit.
- Do not loosen an expected value to make a test pass without explaining why the new value is
  correct.

## 6. Development environment

The C++ tools will use CMake and Ninja against Qt 6, with a Nix flake as the reference
environment, as ld-decode-tools does. Nothing builds yet.

- In-source builds are rejected; use `build/` or `build-*/`.

## 7. CI/CD

None yet. When it exists it will follow ld-decode-tools: a Nix build and test on every push,
and packages for Linux, macOS and Windows. Before proposing build-system or dependency
changes, say in the PR what was validated locally and what only CI can show.

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
