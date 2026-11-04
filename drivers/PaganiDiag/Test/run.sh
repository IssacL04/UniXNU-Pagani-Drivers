#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# PaganiDiag host tests: the core (DiagCore.cpp) and the kernel side's pure C (kernel/pagani_diag.h) with ASan and UBSan at -O0 and
# -O2, then the patch, build, trust bundle, seed and layout checks (Test/check-paganidiag-host.py; a check whose inputs are missing
# says SKIP). Usage: Test/run.sh [check ...]   (no argument: everything)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/paganidiag-test.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
SAN=(-fsanitize=address,undefined -fno-sanitize-recover=all)
for opt in -O0 -O2; do
    xcrun clang++ -std=c++17 $opt -g -Wall -Wextra -Werror "${SAN[@]}" -I "$HERE" "$HERE/DiagCore.cpp" "$HERE/Test/test_core.cpp" \
        -o "$OUT/test_core$opt"
    "$OUT/test_core$opt"
    xcrun clang -std=gnu11 $opt -g -Wall -Wextra -Werror -Wconversion -Wshorten-64-to-32 -Wsign-conversion -Wcast-qual \
        -Wmissing-prototypes -Wstrict-prototypes "${SAN[@]}" "$HERE/Test/test_kernel.c" -o "$OUT/test_kernel$opt"
    "$OUT/test_kernel$opt"
done
python3 -B "$HERE/Test/check-paganidiag-host.py" "$@"
