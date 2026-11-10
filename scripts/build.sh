#!/bin/bash
# Build zwrt-devui with the local Bootlin aarch64 musl toolchain.
# Usage: wsl -- bash -lc 'bash /mnt/d/.../scripts/build.sh'
#        BACKEND=ubus bash scripts/build.sh    (the build for plain OpenWrt, see src/data_ubus.c)
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TC="$HOME/aarch64--musl--stable-2025.08-1/bin"
CC="$TC/aarch64-linux-gcc"
 CXX="$TC/aarch64-linux-g++"
cd "$ROOT"

if [ ! -x "$CC" ]; then echo "toolchain missing: $CC"; exit 1; fi
if [ ! -x "$CXX" ]; then echo "toolchain missing: $CXX"; exit 1; fi

# Optional: static FreeType for CJK text (built by scripts/_build_freetype.sh).
FT_DIR="$HOME/freetype-musl"
LH_DIR="$HOME/litehtml-musl"
FT_CFLAGS=""; FT_LIB=""
if [ -f "$FT_DIR/lib/libfreetype.a" ]; then
  FT_CFLAGS="-I$FT_DIR/include"
  FT_LIB="$FT_DIR/lib/libfreetype.a"
  echo ">> FreeType: $FT_LIB"
else
  echo "ERROR: freetype library not found at $FT_DIR/lib/libfreetype.a"
  exit 1
fi

if [ -f "$LH_DIR/lib/liblitehtml.a" ]; then
  echo ">> litehtml: $LH_DIR/lib/liblitehtml.a"
else
  echo "ERROR: litehtml static lib not found at $LH_DIR/lib/liblitehtml.a"
  exit 1
fi

CFLAGS="-std=c11 -Os -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter \
  -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
  -I. -Iinclude -Ithird_party/stb $FT_CFLAGS"

# BACKEND=datad (default): the zwrt-datad HTTP/SSE client; BACKEND=ubus: ubus and UCI on plain OpenWrt
case "${BACKEND:-datad}" in
  datad) DATA_SRC=data ;;
  ubus)  DATA_SRC=data_ubus ;;
  *) echo "unknown BACKEND: $BACKEND (datad or ubus)"; exit 1 ;;
esac
echo ">> backend: ${BACKEND:-datad}"

APP_SRCS=(
  backlight
  $DATA_SRC
  devui_ext
  drm_disp
  json
  key_input
  touch_input
  livewp
  selfcheck
  htmlmain
)
APPS=""
for s in "${APP_SRCS[@]}"; do
  APPS="$APPS src/${s}.c"
done

# Private object dir: fixed /tmp paths collide between users and parallel builds.
OBJ_DIR="$(mktemp -d)"
trap 'rm -rf "$OBJ_DIR"' EXIT

echo ">> compiler: $($CC -dumpversion)"
echo ">> app sources: ${#APP_SRCS[@]}"
echo ">> building..."

for f in $APPS; do
  obj="$OBJ_DIR/$(basename "${f%.c}").o"
  # shellcheck disable=SC2086
  $CC $CFLAGS -pthread -c "$f" -o "$obj"
  OBJS="$OBJS $obj"
done

CXXFLAGS="-std=c++17 -O2 -w -Iinclude -I$FT_DIR/include -I$LH_DIR/include -I$LH_DIR/include/litehtml -Ithird_party/stb"
"$CXX" $CXXFLAGS -c src/html_view.cpp -o "$OBJ_DIR/html_view.o"
OBJS="$OBJS $OBJ_DIR/html_view.o"

# shellcheck disable=SC2086
"$CXX" -static $OBJS "$LH_DIR/lib/liblitehtml.a" "$FT_LIB" -Wl,--gc-sections -pthread -lm -o zwrt-devui
echo ">> link OK"
"$TC/aarch64-linux-size" zwrt-devui
"$TC/aarch64-linux-strip" -o zwrt-devui.stripped zwrt-devui 2>/dev/null || true
ls -lh zwrt-devui zwrt-devui.stripped 2>/dev/null
echo "BUILD-OK"
