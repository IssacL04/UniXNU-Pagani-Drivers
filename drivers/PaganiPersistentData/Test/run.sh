#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host tests of PaganiPersistentData: format/store/gate/self-test (test_datafile), the NativeUfs glue over a mock transport
# (test_ufs_glue), NativeRamBlock's big-disk volatile layer (test_bigvolatile), all with ASan+UBSan; a kernel-style compile of the
# headers XNU would include; and `git apply --check` of the integration patches against the current tree. Touches no device and
# writes nothing outside a temporary directory.   Usage: Test/run.sh [--keep DIR]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
DRIVERS=$(dirname "$ROOT")
RAMBLOCK="$DRIVERS/NativeRamBlock"
OUT=$(mktemp -d "${TMPDIR:-/tmp}/pdata-test.XXXXXX")
if [ "${1:-}" = "--keep" ]; then OUT=$2; mkdir -p "$OUT"; else trap 'rm -rf "$OUT"' EXIT; fi
CXX=${CXX:-c++}
SAN=(-std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined)

echo '== test_datafile (format, slots, FIEMAP, map, gate, canaries, read-back, self-test, end to end, simulated 128 GiB)'
"$CXX" "${SAN[@]}" -I"$ROOT" -I"$RAMBLOCK" "$HERE/test_datafile.cpp" -o "$OUT/test_datafile"
(cd "$HERE" && "$OUT/test_datafile" sim-128g-hint0.txt) | tee "$OUT/test_datafile.log" | tail -6
"$OUT/test_datafile" --emit "$OUT/slot.bin" > "$OUT/vectors.txt"
python3 -I - "$OUT/test_datafile.log" <<'EOF'
import hashlib, re, sys
log = open(sys.argv[1]).read()
want = hashlib.sha256(b'a' * 1000003).hexdigest()
got = re.search(r'sha256\(a\*1000003\)=([0-9a-f]{64})', log).group(1)
assert got == want, (got, want)
print('sha256 cross-check with hashlib: ok')
EOF

echo '== test_ufs_glue (DataFileUfs.h over a mock NativeUfs transport, NativeRamBlock overlay on top)'
"$CXX" "${SAN[@]}" -I"$ROOT" -I"$RAMBLOCK" "$HERE/test_ufs_glue.cpp" -o "$OUT/test_ufs_glue"
"$OUT/test_ufs_glue"

echo '== test_bigvolatile (RAM layer of a big Data disk: fallback and read-only verification)'
"$CXX" "${SAN[@]}" -Wno-c99-extensions -I"$ROOT" -I"$RAMBLOCK" "$HERE/test_bigvolatile.cpp" -o "$OUT/test_bigvolatile"
"$OUT/test_bigvolatile"

echo '== kernel-style compile (-ffreestanding -fno-exceptions -fno-rtti -nostdinc++, -Wall -Werror incl. unused functions): patch hooks over DataFileUfs.h, BigVolatile.h'
cat > "$OUT/kernel.cpp" <<'EOF'
#include <stddef.h>
#include <stdint.h>
namespace transport {
typedef int IOReturn;
enum : int { kIOReturnSuccess = 0 };
enum : uint32_t { kDataMax = 131072 };
struct UfsPartition { uint32_t lun; uint64_t first, blocks; };
enum : int { UfsPartOk = 0 };
static constexpr uint32_t kUfsPartScratch = 4096 + 128 * 128;
struct SHA256_CTX { uint64_t opaque[16]; };
void SHA256_Init (SHA256_CTX *);
void SHA256_Update (SHA256_CTX *, const void *, size_t);
void SHA256_Final (void *, SHA256_CTX *);
uint64_t mach_absolute_time ();
bool PE_parse_boot_argn (const char *, void *, int);
void *IOMallocData (size_t);
void IOFreeData (void *, size_t);
IOReturn hostRead4k (uint8_t, uint64_t, uint32_t, void *);
IOReturn hostWrite4k (uint8_t, uint64_t, uint32_t, const void *, bool);
IOReturn hostSyncCache ();
int ufsPartFind (const char *, UfsPartition *, uint8_t *);
static bool dataFileWriteAllowed (uint64_t lba, uint32_t blocks);
static bool dataFileWritesArmed ();
#include "DataFileUfs.h"
bool hostInWindowHook (uint64_t lba, uint32_t blocks) { return dataFileWriteAllowed (lba, blocks) && dataFileWritesArmed (); }
int open (uint64_t b, const DataFileRoot &r, uint64_t *f, const char **w) { return dataFileStoreOpen (b, r, f, w) + dataFileStoreRead (0, 1, nullptr) + dataFileStoreWrite (0, 1, nullptr) + dataFileStoreSync (); }
}
#include "BigVolatile.h"
struct Ops { uint8_t *allocate (); void release (uint8_t *); void **table (uint64_t); void untable (void **, uint64_t); };
struct Base { bool page (uint64_t, uint8_t *); };
struct Buf { uint64_t read (uint64_t, void *, uint64_t); uint64_t copy (uint64_t, const void *, uint64_t); };
uint64_t big (BigVolatile &v, Base &b, Ops &o, uint8_t *s, Buf &f, bool *m) { v.release (o); return bigVolatileIO (v, b, o, s, true, 0, 1, f, m); }
EOF
"$CXX" -std=c++17 -ffreestanding -fno-exceptions -fno-rtti -nostdinc++ -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
    -I"$ROOT" -c "$OUT/kernel.cpp" -o "$OUT/kernel.o"
echo 'ok'

echo '== integration/NativeUfs.patch on a temporary copy: the real RootCommands.h command path + DataFileUfs.h over a device model'
mkdir -p "$OUT/tree/drivers"
cp -R "$DRIVERS/NativeUfs" "$ROOT" "$OUT/tree/drivers/"
(cd "$OUT/tree" && git apply -p1 "$ROOT/integration/NativeUfs.patch")
cp "$HERE/test_ufs_patch.cpp" "$OUT/tree/drivers/NativeUfs/tests/"
for flags in "-DNATIVE_UFS_READ_ONLY=1 -DNATIVE_UFS_DATA_FILE=1" "-DNATIVE_UFS_READ_ONLY=1 -DNATIVE_UFS_DATA_FILE=1 -DNATIVE_UFS_SM8750=1"; do
    # shellcheck disable=SC2086
    (cd "$OUT/tree/drivers/NativeUfs/tests" && "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
        -fsanitize=address,undefined -fno-sanitize-recover=undefined $flags test_ufs_patch.cpp -o "$OUT/test_ufs_patch")
    "$OUT/test_ufs_patch"
done
echo '== the unchanged NativeUfs host test of the command path still passes with the patched RootCommands.h (flag off)'
(cd "$OUT/tree/drivers/NativeUfs/tests" && "$CXX" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -DNATIVE_UFS_SM8750=1 -DNATIVE_UFS_READ_ONLY=1 RootCommands.cpp -o "$OUT/root_commands")
"$OUT/root_commands" | tail -1

echo '== integration/NativeRamBlock.patch on a temporary copy: -fsyntax-only against declaration stubs (Test/kstub), before and after'
cp -R "$DRIVERS/NativeRamBlock" "$OUT/tree/drivers/"
"$CXX" -std=c++17 -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter -I"$HERE/kstub" "$OUT/tree/drivers/NativeRamBlock/NativeRamBlock.cpp"
(cd "$OUT/tree" && git apply -p1 "$ROOT/integration/NativeRamBlock.patch")
"$CXX" -std=c++17 -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter -I"$HERE/kstub" "$OUT/tree/drivers/NativeRamBlock/NativeRamBlock.cpp"
echo 'ok'

echo '== integration patches apply to the current tree (git apply --check, nothing is applied)'
REPO=$(cd "$DRIVERS/.." && git rev-parse --show-toplevel 2>/dev/null || true)
if [ -n "$REPO" ]; then
    for p in "$ROOT"/integration/*.patch; do
        (cd "$REPO" && git apply --check "$p") && echo "applies: $(basename "$p")"
    done
else
    echo 'skip: not inside the drivers git repository'
fi
echo 'ALL PASS'
