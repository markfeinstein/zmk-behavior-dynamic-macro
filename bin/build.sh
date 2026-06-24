#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Behavior Dynamic Macro Contributors
# SPDX-License-Identifier: MIT

set -euo pipefail

repo_dir=${ZBDM_REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)}
repo_dir=$(realpath "$repo_dir")

normalize_extra_modules() {
    local repo_real modules path real extra_modules sep repo_included

    repo_real=$(realpath "$repo_dir")
    modules=${ZMK_EXTRA_MODULES:-}
    extra_modules=
    sep=

    if [[ -n $modules ]]; then
        IFS=';' read -r -a module_paths <<<"$modules"
        for path in "${module_paths[@]}"; do
            [[ -z $path ]] && continue
            real=$(realpath "$path")
            if [[ $real == "$repo_real" ]]; then
                repo_included=1
            fi
            extra_modules+="$sep$real"
            sep=';'
        done
    fi

    if [[ -z ${repo_included:-} ]]; then
        extra_modules+="$sep$repo_real"
    fi

    printf '%s\n' "$extra_modules"
}

if [[ -z ${ZMK_APP_DIR:-} ]]; then
    for candidate in \
        "$repo_dir/../zmk/app" \
        "$repo_dir/../zmkfirmware/zmk/app" \
        "$repo_dir/.west-workspace/zmk/app"; do
        if [[ -f "$candidate/CMakeLists.txt" && -x "$candidate/run-test.sh" ]]; then
            ZMK_APP_DIR=$candidate
            break
        fi
    done
fi

if [[ -z ${ZMK_APP_DIR:-} || ! -f "$ZMK_APP_DIR/CMakeLists.txt" ]]; then
    cat >&2 <<'EOF'
Unable to find the ZMK app directory.

Set ZMK_APP_DIR to a ZMK app checkout, for example:
  ZMK_APP_DIR=/path/to/zmk/app ZMK_CONFIG=/path/to/config BOARD='nice_nano_v2' nix run .#build
EOF
    exit 1
fi

if [[ -z ${ZMK_CONFIG:-} || -z ${BOARD:-} ]]; then
    cat >&2 <<'EOF'
ZMK_CONFIG and BOARD are required.

Example:
  ZMK_APP_DIR=/path/to/zmk/app \
  ZMK_CONFIG=/path/to/zmk-config/config \
  BOARD='nice_nano_v2' \
  nix run .#build
EOF
    exit 1
fi

build_dir=${ZMK_BUILD_DIR:-"$PWD/build/$BOARD"}
extra_modules=$(normalize_extra_modules)

west build \
    -s "$ZMK_APP_DIR" \
    -d "$build_dir" \
    -b "$BOARD" \
    -p -- \
    -DZMK_CONFIG="$(realpath "$ZMK_CONFIG")" \
    -DZMK_EXTRA_MODULES="$extra_modules" \
    "$@"
