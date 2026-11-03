# Contributing

Thanks for helping keep RF-tools working.

## Before you start

- Search existing issues and pull requests to avoid duplicates.
- For anything that changes the `.rfx` format or a tool's command-line options, open an issue
  first.

## Workflow

1. Fork and create a branch named after what it does (not `fix2`, `new`, `final`).
2. Build and test: see [AGENTS.md](AGENTS.md) §5 and §6.
3. Keep the change focused; do not reformat code you are not otherwise changing.
4. New source files carry an SPDX header (see [AGENTS.md](AGENTS.md) §4.2).
5. A change to the format updates [docs/Specifications/rfx-format.md](docs/Specifications/rfx-format.md)
   in the same pull request.
6. Open a pull request using the template and say what you validated and on which platforms.

## Commit messages

Describe the change and why, reference issues where relevant. Do not add tool or AI
attribution trailers (see [AGENTS.md](AGENTS.md), Rule 2).

## Licence

By contributing you agree that your contribution is licensed under the
[GNU GPL v3 or later](LICENSE).
