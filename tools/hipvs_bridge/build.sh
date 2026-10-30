#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
PYTHON_BIN=${PYTHON_BIN:-python3}
OUTPUT=${1:-$SCRIPT_DIR/libseekdb_cuvs_bridge.so}

[[ -x "$HIPCC" ]] || { echo "hipcc is not executable: $HIPCC" >&2; exit 2; }
[[ -f "$HIPVS_ROOT/include/cuvs/core/c_api.h" ]] || {
  echo "hipVS C headers are missing under $HIPVS_ROOT/include" >&2; exit 2;
}
for lib in libcuvs_c.so libcuvs.so librmm.so librapids_logger.so; do
  [[ -f "$HIPVS_ROOT/lib/$lib" ]] || { echo "missing $HIPVS_ROOT/lib/$lib" >&2; exit 2; }
done

# Compile against pinned public headers without downloading or replacing any
# installed hipVS/ROCm libraries. The archive contains source headers/licenses.
scratch=$(mktemp -d "${TMPDIR:-/tmp}/seekdb-cuvs-build.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
"$PYTHON_BIN" -m zipfile -e "$SCRIPT_DIR/cpp_headers.zip" "$scratch/headers"

common=(-fPIC -O2 -D__HIP_PLATFORM_AMD__ -DLIBCUDACXX_ENABLE_EXPERIMENTAL_MEMORY_RESOURCE
  -I"$scratch/headers/hipvs" -I"$HIPVS_ROOT/include"
  -I"$scratch/headers/hipraft" -I"$scratch/headers/hipmm"
  -I"$scratch/headers/logger" -I"$scratch/headers/libhipcxx" -I"$ROCM_ROOT/include")
"$HIPCC" "${common[@]}" -c "$SCRIPT_DIR/seekdb_cuvs_bridge.c" -o "$scratch/bridge.o"
"$HIPCC" "${common[@]}" -std=c++17 -c "$SCRIPT_DIR/seekdb_cuvs_bounded_build.cpp" \
  -o "$scratch/bounded_build.o"

mkdir -p "$(dirname "$OUTPUT")"
"$HIPCC" -shared "$scratch/bridge.o" "$scratch/bounded_build.o" \
  -L"$HIPVS_ROOT/lib" -lcuvs_c -lcuvs -lrmm -lrapids_logger -pthread \
  -Wl,--no-undefined -Wl,-soname,libseekdb_cuvs_bridge.so \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" -o "$OUTPUT"
echo "built $OUTPUT"
