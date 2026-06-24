#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK Behavior Dynamic Macro Contributors
# SPDX-License-Identifier: MIT

set -euo pipefail

repo_dir=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)}
cd "$repo_dir"

legacy_pattern='CONFIG_ZMK_DYNAMIC_MACRO|zmk_dynamic_macro_|(^|[^A-Z])DM_(RECORD|PLAY|STOP|TOGGLE|CLEAR|CANCEL|REC|PLY|STP)|DYN_MACRO|zmk,behavior-dynamic-macro|zmk_behavior_dynamic_macro|<behaviors/dynamic_macro\.dtsi>|<dt-bindings/zmk/dynamic_macro\.h>|<zmk/dynamic_macro\.h>|&dm([^[:alnum:]_]|$)|&dyn_macro([^[:alnum:]_]|$)|dynamic_macro/slot_|zmk-dynamic-macro|tests/dynamic-macro|modules/zmk/dynamic-macro'

mapfile -t files < <(
    find . -type f \
        -not -path './.git/*' \
        -not -path './.pi-subagents/*' \
        -not -path './.west-workspace/*' \
        -not -path './README.md' \
        -not -path './bin/check-namespace.sh' \
        -print
)
if grep -nEI "$legacy_pattern" "${files[@]}"; then
    echo "Legacy dynamic-macro namespace remains outside the migration notes" >&2
    exit 1
fi
