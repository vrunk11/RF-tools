{
  description = "RF-tools (Nix flake)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        packageVersion = "0.1.0";
        rev = if self ? rev then self.rev else "";
        shortRev = if self ? shortRev then self.shortRev else (if rev != "" then builtins.substring 0 7 rev else "unknown");
        dirtySuffix = if self ? dirtyRev then "-dirty" else "";
        branch = if self ? ref then self.ref else "nix";
        nixCommit = "${shortRev}${dirtySuffix}";
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "rf-tools";
          version = packageVersion;
          src = pkgs.lib.cleanSourceWith {
            src = ./.;
            filter = path: type:
              let
                base = pkgs.lib.baseNameOf path;
              in
                !(base == ".git" || base == "build" || base == "result");
          };

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            qt6.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            qt6.qtbase
            qt6.qtsvg
            libGL
          ];

          cmakeBuildType = "Release";
          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DAPP_BRANCH=${branch}"
            "-DAPP_COMMIT=${nixCommit}"
          ];
        };

        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            qt6.qtbase
            qt6.qtsvg
            libGL
            ffmpeg
            # Compile cache used by CI (CMAKE_CXX_COMPILER_LAUNCHER=ccache);
            # harmless when unused locally.
            ccache
          ];
          shellHook = ''
            export QT_PLUGIN_PATH=${pkgs.qt6.qtsvg}/lib/qt-6/plugins''${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}
          '';
        };
      }
    );
}
