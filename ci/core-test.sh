#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Contract tests of the tsf-geospatial core, without Test Environment:
# build ci/core_test with ASan/UBSan (ci/build-core.sh) and run it in a
# scratch directory. Exits non-zero on the first broken contract or the
# first sanitizer report (leaks included).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${OUT:-build-ci}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
OUT="$OUT" ./ci/build-core.sh
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}" \
UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}" \
    "$OUT/core_test" "$W"
