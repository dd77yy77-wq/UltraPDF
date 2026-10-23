#!/bin/bash
# Cross-compiles UltraPDF.exe (x64, static, one file) with llvm-mingw.
# Usage: TC=/path/to/llvm-mingw PDFIUM_INC=/path/to/pdfium/include ./build.sh [out.exe]
set -e
cd "$(dirname "$0")"
TC=${TC:-$(ls -d /tmp/wl/llvm-mingw-* | head -1)}
INC=${PDFIUM_INC:-/tmp/wl/pdfium/include}
OUT=${1:-UltraPDF.exe}
CXX=$TC/bin/x86_64-w64-mingw32-clang++
RC=$TC/bin/x86_64-w64-mingw32-windres
mkdir -p obj
$RC -I. app.rc -O coff -o obj/app.res.o
OBJS=obj/app.res.o
pids=()
for f in src/*.cpp; do
  o=obj/$(basename $f .cpp).o
  if [ ! -f $o ] || [ $f -nt $o ] || [ src/app.h -nt $o ]; then
    $CXX -O2 -std=c++17 -DUNICODE -D_UNICODE -Wall -Wno-unused-function -Wno-missing-braces -Wno-unused-variable -I"$INC" -Isrc -c $f -o $o &
    pids+=($!)
  fi
  OBJS="$OBJS $o"
done
fail=0
for p in "${pids[@]}"; do wait $p || fail=1; done
[ $fail = 0 ] || exit 1
$CXX -O2 -static -mwindows -municode -o "$OUT" $OBJS -lgdiplus -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lwinhttp -ladvapi32 -ldwmapi -luxtheme -lole32 -luser32 -lkernel32 -lmsimg32 -lwinspool -ldxguid
$TC/bin/x86_64-w64-mingw32-strip "$OUT"
ls -l "$OUT"
