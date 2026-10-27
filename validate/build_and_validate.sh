#!/usr/bin/env bash
# Build the rv_dsp core into a shared library and run the validation harness.
# Prereqs: a C compiler (cc/clang/gcc, or `python -m ziglang cc`) and Python with
# numpy + matplotlib.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
INC="$ROOT/firmware/components/rv_dsp/include"
SRC="$ROOT/firmware/components/rv_dsp/rv_dsp.c"
SHIM="$HERE/rv_shim.c"
OUT="$ROOT/build/librvsim.so"
mkdir -p "$ROOT/build"

PY="${PYTHON:-python3}"

build_with() { "$@" -std=c99 -O2 -fPIC -shared -I"$INC" "$SHIM" "$SRC" -lm -o "$OUT"; }

if command -v cc >/dev/null 2>&1 && build_with cc 2>/dev/null; then
    echo "built $OUT with cc"
elif command -v clang >/dev/null 2>&1 && build_with clang 2>/dev/null; then
    echo "built $OUT with clang"
elif command -v gcc >/dev/null 2>&1 && build_with gcc 2>/dev/null; then
    echo "built $OUT with gcc"
elif "$PY" -c "import ziglang" 2>/dev/null; then
    build_with "$PY" -m ziglang cc
    echo "built $OUT with ziglang"
else
    echo "no usable C compiler found (cc/clang/gcc/ziglang)" >&2
    exit 1
fi

cd "$HERE"
exec "$PY" rv_validate.py
