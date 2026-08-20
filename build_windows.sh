#!/usr/bin/env bash
# Cross-compiles customDB for Windows (x86_64) using mingw-w64.
#
# Requires: x86_64-w64-mingw32-gcc (posix threading variant, for pthread
# support). On Debian/Ubuntu:
#   sudo apt-get install mingw-w64
#   sudo update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
#
# Produces, in build_win/:
#   db.exe          - CLI REPL (statically linked, no runtime DLLs needed)
#   db_server.exe   - TCP SQL server (statically linked, needs ws2_32 which
#                     ships with every Windows install)
#   dbms.dll        - engine as a shared library
#   libdbms.dll.a   - import lib for linking dbms.dll from MinGW/GCC

set -euo pipefail
CC=x86_64-w64-mingw32-gcc
OUT=build_win
CFLAGS="-Wall -Wextra -Iinclude -O2 -D_WIN32"
CORE_SRCS="pager catalog btree cursor parser vdbe executor"

rm -rf "$OUT"
mkdir -p "$OUT"

echo "== Compiling core engine =="
for f in $CORE_SRCS; do
  $CC $CFLAGS -c "src/$f.c" -o "$OUT/$f.o"
done

echo "== Compiling api.c (static mode, for db.exe / db_server.exe) =="
$CC $CFLAGS -DDBMS_STATIC -c src/api.c -o "$OUT/api.o"

echo "== Compiling api.c (DLL export mode, for dbms.dll) =="
$CC $CFLAGS -DDBMS_BUILD_DLL -c src/api.c -o "$OUT/api_dll.o"

echo "== Compiling main.c =="
$CC $CFLAGS -c src/main.c -o "$OUT/main.o"

echo "== Compiling server.c (static mode) =="
$CC $CFLAGS -DDBMS_STATIC -c src/server.c -o "$OUT/server.o"

CORE_OBJS="$OUT/pager.o $OUT/catalog.o $OUT/btree.o $OUT/cursor.o $OUT/parser.o $OUT/vdbe.o $OUT/executor.o $OUT/api.o"
DLL_OBJS="$OUT/pager.o $OUT/catalog.o $OUT/btree.o $OUT/cursor.o $OUT/parser.o $OUT/vdbe.o $OUT/executor.o $OUT/api_dll.o"

echo "== Linking db.exe =="
$CC -O2 -o "$OUT/db.exe" $CORE_OBJS "$OUT/main.o" -static -lpthread

echo "== Linking db_server.exe =="
$CC -O2 -o "$OUT/db_server.exe" $CORE_OBJS "$OUT/server.o" -static -lpthread -lws2_32

echo "== Linking dbms.dll =="
$CC -shared -O2 -static -o "$OUT/dbms.dll" $DLL_OBJS -lpthread -Wl,--out-implib,"$OUT/libdbms.dll.a"

echo
echo "Done. Artifacts in $OUT/:"
ls -la "$OUT"/*.exe "$OUT"/*.dll "$OUT"/*.dll.a
