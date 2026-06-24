#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Behavior Dynamic Macro Contributors
# SPDX-License-Identifier: MIT

set -euo pipefail

repo_dir=${ZBDM_REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)}
repo_dir=$(realpath "$repo_dir")

if [[ $(uname -s) != Linux ]]; then
    cat >&2 <<'EOF'
ZMK native_sim tests require Zephyr's POSIX architecture, which only builds on Linux.
Run this through Nix on a Linux machine or Linux VM.
EOF
    exit 2
fi

if (($# > 0)); then
    testcases=("$@")
else
    testcases=()
    while IFS= read -r testcase; do
        testcases+=("$testcase")
    done < <(find "$repo_dir/tests/zmk-behavior-dynamic-macro" -mindepth 1 -maxdepth 1 -type d | sort)
fi

if ((${#testcases[@]} == 0)); then
    cat >&2 <<EOF
No dynamic macro test cases found under:
  $repo_dir/tests/zmk-behavior-dynamic-macro
EOF
    exit 1
fi

for i in "${!testcases[@]}"; do
    if [[ ${testcases[i]} != /* ]]; then
        testcases[i]="$repo_dir/${testcases[i]}"
    fi
done

if [[ -z ${ZMK_APP_DIR:-} ]]; then
    for candidate in \
        "$repo_dir/../zmk/app" \
        "$repo_dir/../zmkfirmware/zmk/app" \
        "$repo_dir/.west-workspace/zmk/app"; do
        if [[ -x "$candidate/run-test.sh" ]]; then
            ZMK_APP_DIR=$candidate
            break
        fi
    done
fi

if [[ -z ${ZMK_APP_DIR:-} || ! -x "$ZMK_APP_DIR/run-test.sh" ]]; then
    cat >&2 <<EOF
Unable to find ZMK app/run-test.sh.

Set ZMK_APP_DIR to a ZMK app checkout, for example:
  ZMK_APP_DIR=/path/to/zmk/app nix run .#test
EOF
    exit 1
fi

repo_real=$(realpath "$repo_dir")
if [[ -n ${ZMK_EXTRA_MODULES:-} ]]; then
    if [[ $ZMK_EXTRA_MODULES == *';'* ]]; then
        cat >&2 <<'EOF'
ZMK run-test.sh supports only one ZMK_EXTRA_MODULES path.
Unset ZMK_EXTRA_MODULES before running this module's tests.
EOF
        exit 1
    fi

    if ! extra_modules_real=$(realpath "$ZMK_EXTRA_MODULES" 2>/dev/null); then
        cat >&2 <<EOF
Unable to resolve ZMK_EXTRA_MODULES: $ZMK_EXTRA_MODULES
EOF
        exit 1
    fi

    if [[ $extra_modules_real != "$repo_real" ]]; then
        cat >&2 <<EOF
ZMK run-test.sh supports only one ZMK_EXTRA_MODULES path.
Unset ZMK_EXTRA_MODULES or set it to this module:
  ZMK_EXTRA_MODULES=$repo_real
EOF
        exit 1
    fi
fi

cd "$ZMK_APP_DIR"
for testcase in "${testcases[@]}"; do
    printf 'Running ZMK native_sim test: %s\n' "$testcase"
    if ! ZMK_EXTRA_MODULES=$repo_real ./run-test.sh "$testcase"; then
        rel_testcase=$(realpath "$testcase" | sed -n -e 's|.*/tests/||p')
        build_log="${ZMK_BUILD_DIR:-$ZMK_APP_DIR/build}/tests/$rel_testcase/build.log"
        if [[ -f $build_log ]]; then
            cat "$build_log" >&2
        fi
        exit 1
    fi
done
