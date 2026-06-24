# SPDX-License-Identifier: MIT

{
  description = "Nix shell, build, and test helpers for the zmk-behavior-dynamic-macro ZMK module";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    # Keep this in sync with the Zephyr generation used by current ZMK main.
    zephyr-nix.url = "github:urob/zephyr-nix/62288a26868d805935952a8cf8a77ffe5ec994f7";
    zephyr-nix.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs =
    {
      self,
      nixpkgs,
      zephyr-nix,
      ...
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f {
            pkgs = nixpkgs.legacyPackages.${system};
            zephyrPkgs = zephyr-nix.packages.${system};
          }
        );
    in
    {
      checks = forAllSystems (
        { pkgs, ... }:
        {
          shell-scripts =
            pkgs.runCommand "zmk-behavior-dynamic-macro-shell-scripts"
              {
                nativeBuildInputs = [
                  pkgs.bash
                  pkgs.shellcheck
                ];
              }
              ''
                bash -n ${./bin/build.sh} ${./bin/test.sh} ${./bin/check-namespace.sh}
                shellcheck ${./bin/build.sh} ${./bin/test.sh} ${./bin/check-namespace.sh}
                bash ${./bin/check-namespace.sh} ${./.}
                touch "$out"
              '';
        }
      );

      packages = forAllSystems (
        { pkgs, zephyrPkgs }:
        let
          pythonEnv = pkgs.python3.withPackages (
            ps: with ps; [
              west
              pyelftools
              pyyaml
              protobuf
              setuptools
            ]
          );

          sdk = zephyrPkgs.sdk-0_16.override { targets = [ "arm-zephyr-eabi" ]; };
          pythonPath = "${pythonEnv}/${pkgs.python3.sitePackages}";
          zephyrTools = [
            pkgs.cacert
            pkgs.cmake
            pkgs.coreutils
            pkgs.diffutils
            pkgs.dtc
            pkgs.findutils
            pkgs.git
            pkgs.gnused
            pkgs.ninja
            pkgs.protobuf
            pkgs.stdenv.cc
            pythonEnv
            sdk
          ];

          wrapZephyrScript =
            name: script: defaultToolchainVariant:
            pkgs.writeShellApplication {
              inherit name;
              runtimeInputs = zephyrTools;
              text = ''
                export ZBDM_REPO_DIR="${self}"
                export ZEPHYR_SDK_INSTALL_DIR="${sdk}"
                export ZEPHYR_TOOLCHAIN_VARIANT="''${ZEPHYR_TOOLCHAIN_VARIANT:-${defaultToolchainVariant}}"
                export PYTHONPATH="${pythonPath}''${PYTHONPATH:+:$PYTHONPATH}"
                exec ${script} "$@"
              '';
            };
        in
        rec {
          build = wrapZephyrScript "zmk-behavior-dynamic-macro-build" ./bin/build.sh "zephyr";
          test = wrapZephyrScript "zmk-behavior-dynamic-macro-test" ./bin/test.sh "host";
          default = test;
        }
      );

      apps = forAllSystems (
        { pkgs, ... }:
        rec {
          build = {
            type = "app";
            program = "${self.packages.${pkgs.system}.build}/bin/zmk-behavior-dynamic-macro-build";
            meta.description = "Build ZMK firmware with the zmk-behavior-dynamic-macro module";
          };
          test = {
            type = "app";
            program = "${self.packages.${pkgs.system}.test}/bin/zmk-behavior-dynamic-macro-test";
            meta.description = "Run zmk-behavior-dynamic-macro native_sim tests";
          };
          default = test;
        }
      );

      devShells = forAllSystems (
        { pkgs, zephyrPkgs }:
        let
          pythonEnv = pkgs.python3.withPackages (
            ps: with ps; [
              west
              pyelftools
              pyyaml
              protobuf
              setuptools
            ]
          );

          sdk = zephyrPkgs.sdk-0_16.override { targets = [ "arm-zephyr-eabi" ]; };
          pythonPath = "${pythonEnv}/${pkgs.python3.sitePackages}";
        in
        rec {
          zephyr = pkgs.mkShellNoCC {
            packages = [
              pkgs.cmake
              pkgs.dtc
              pkgs.git
              pkgs.ninja
              pkgs.protobuf
              pkgs.stdenv.cc
              pythonEnv
              sdk
            ];
            env = {
              ZEPHYR_SDK_INSTALL_DIR = "${sdk}";
              ZEPHYR_TOOLCHAIN_VARIANT = "zephyr";
              PYTHONPATH = pythonPath;
            };
          };

          checks = pkgs.mkShellNoCC {
            packages = [
              pkgs.bash
              pkgs.shellcheck
            ];
          };

          default = zephyr;
        }
      );

      formatter = forAllSystems ({ pkgs, ... }: pkgs.nixfmt-rfc-style);
    };
}
