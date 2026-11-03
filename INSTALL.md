# Installation (Nix)

This project ships a Nix flake that provides both a build and a development environment. The simplest way to install is with Nix flakes.

## Prerequisites

- Nix with flakes enabled.
  - Ensure `experimental-features = nix-command flakes` is set in your Nix config.

## Install with Nix

### Install into your user profile

```bash
nix profile install .#
```

This installs the program into your Nix profile (for example, `~/.nix-profile/bin`).

### Build without installing

```bash
nix build .#
```

The build output will be available under `./result`.

### Run from the build output

```bash
./result/bin/rfx-gui
```

## Other platforms

Packages for Linux (tarball and AppImage), macOS (DMG) and Windows (ZIP) are built by the CI for every
push and attached to each GitHub release.

## Notes

- The flake pins all required dependencies, including Qt 6.
- Build metadata (branch/commit) is set by the flake during the build.
