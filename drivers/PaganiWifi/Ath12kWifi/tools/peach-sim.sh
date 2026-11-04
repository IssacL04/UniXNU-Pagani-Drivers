#!/bin/bash
# The whole stack against tests/stack_sim.c on the host, the peach entry's cold-boot calibration cases and the original WCN7850 run: builds everything
# from the trees tools/prepare.sh left (it runs it first) with AddressSanitizer, so a power down that leaves a stale pointer behind fails here.
# Needs deps/ (tools/fetch-sources.sh). Objects and logs go to $1 (default out/sim). Exit 0 when every case passes.
cd "$(dirname "$0")/.." || exit 2
OUT=${1:-out/sim}
case "$OUT" in /*) ;; *) OUT=$PWD/$OUT ;; esac
bash tools/prepare.sh > /dev/null || exit 2
rm -rf "$OUT"; mkdir -p "$OUT/obj"
RES=$(clang -print-resource-dir)/include
CF="-c -nostdinc -isystem shim/libc -isystem $RES -std=gnu11 -w -g -fsanitize=address -fno-omit-frame-pointer -DCONFIG_ATH12K_DEBUG -include lx_pci.h -I shim/include -I include -I out/pinned -I src -I out/ath12k-src"
A=out/ath12k-src
compile() { clang $CF "$@" 2>&1 | grep -E "error" | head -3; }
for f in $A/*.c $A/wifi7/*.c; do
  b=$(basename "$f" .c); d=$(basename "$(dirname "$f")")
  case $b in debugfs*|testmode|ahb|trace|acpi|coredump|wow|debug) continue;; esac
  [ "$d" = wifi7 ] && b=w7_$b
  compile -I $A/wifi7 "$f" -o "$OUT/obj/$b.o" &
  while [ "$(jobs -r | wc -l)" -ge 8 ]; do sleep 0.05; done
done
for f in src/*.c shim/src/*.c; do compile -I out/mhi-src "$f" -o "$OUT/obj/$(basename "$f" .c).o" & done
for f in init main pm boot; do compile -I out/mhi-src out/mhi-src/$f.c -o "$OUT/obj/mhi_$f.o" & done
for f in qmi_interface qmi_encdec; do compile deps/linux/drivers/soc/qcom/$f.c -o "$OUT/obj/$f.o" & done
wait
clang -std=gnu11 -w -g -fsanitize=address -fno-omit-frame-pointer -DCONFIG_ATH12K_DEBUG -include lx_pci.h -I shim/include -I include -I out/pinned -I src -I out/ath12k-src \
  tests/stack_sim.c $(ls "$OUT"/obj/*.o | grep -v '/qmi.o') -o "$OUT/sim" 2>&1 | grep -E "error|undefined|Undefined" | head
[ -x "$OUT/sim" ] || { echo "peach-sim: the simulation did not build" >&2; exit 2; }
fail=0
export ASAN_OPTIONS=detect_leaks=0
"$OUT/sim" > "$OUT/wcn7850.log" 2>&1 || { echo "FAIL wcn7850: exit $?"; fail=1; }
grep -q 'after run: stage=2 scans=0 err=0 dev-stage=4' "$OUT/wcn7850.log" && echo "PASS wcn7850 (the run without PEACH)" || { echo "FAIL wcn7850: see $OUT/wcn7850.log"; fail=1; }
for c in ${PEACH_CASES:-host self late stuck dead nocal}; do
  PEACH=$c "$OUT/sim" > "$OUT/$c.log" 2>&1; rc=$?
  grep -E '^(FAIL|peach )' "$OUT/$c.log"
  if [ $rc = 0 ] && grep -q "^PASS peach $c\$" "$OUT/$c.log"; then echo "PASS peach $c"; else echo "FAIL peach $c: exit $rc, see $OUT/$c.log"; fail=1; fi
done
exit $fail
