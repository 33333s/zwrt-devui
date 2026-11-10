#!/bin/bash
# Build the host preview of the DevUI pages (Linux, gcc/g++, git).
# FreeType and litehtml are built natively once, at the revisions pinned in
# scripts/deps-revs.sh, into $DEVUI_PREVIEW_CACHE (default ~/.cache/devui-preview).
# usage: bash tests/ui/build_preview.sh [port]   -> $DEVUI_PREVIEW_CACHE/preview-host
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/scripts/deps-revs.sh"
CACHE="${DEVUI_PREVIEW_CACHE:-$HOME/.cache/devui-preview}"
PORT="${1:-29462}"
JOBS="$(nproc 2>/dev/null || echo 4)"
mkdir -p "$CACHE/src"

fetch_rev() {
    if [ "$(git -C "$2" rev-parse HEAD 2>/dev/null)" = "$3" ]; then return 0; fi
    rm -rf "$2"
    git init -q "$2"
    git -C "$2" fetch -q --depth 1 "$1" "$3"
    git -C "$2" checkout -q FETCH_HEAD
}

fetch_rev https://github.com/freetype/freetype.git "$CACHE/src/freetype" "$FREETYPE_REV"
fetch_rev https://github.com/litehtml/litehtml.git "$CACHE/src/litehtml" "$LITEHTML_REV"

FT="$CACHE/src/freetype"
LH="$CACHE/src/litehtml"

# same module set as scripts/_build_freetype.sh, built with the host compiler
if [ "$(cat "$CACHE/freetype.rev" 2>/dev/null)" != "$FREETYPE_REV" ]; then
    B="$CACHE/ft-build"; rm -rf "$B"; mkdir -p "$B"
    cat > "$B/ftmodule_min.h" <<'MOD'
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
MOD
    for s in base/ftbase base/ftsystem base/ftinit base/ftdebug base/ftbbox base/ftbitmap \
             base/ftglyph base/ftmm cache/ftcache autofit/autofit truetype/truetype cff/cff \
             psaux/psaux psnames/psnames pshinter/pshinter sfnt/sfnt smooth/smooth gzip/ftgzip; do
        gcc -O2 -DFT2_BUILD_LIBRARY -I"$FT/include" -I"$B" -DFT_CONFIG_MODULES_H='"ftmodule_min.h"' \
            -c "$FT/src/$s.c" -o "$B/$(echo "$s" | tr / _).o" &
        while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do wait -n; done
    done
    wait
    ar rcs "$CACHE/libfreetype.a" "$B"/*.o
    echo "$FREETYPE_REV" > "$CACHE/freetype.rev"
fi

if [ "$(cat "$CACHE/litehtml.rev" 2>/dev/null)" != "$LITEHTML_BUILD" ]; then
    git -C "$LH" checkout -q -- .
    git -C "$LH" apply "$LITEHTML_PATCH"
    B="$CACHE/lh-build"; rm -rf "$B"; mkdir -p "$B"
    GI="-I$LH/src/gumbo/include -I$LH/src/gumbo/include/gumbo"
    for f in "$LH"/src/gumbo/*.c; do
        gcc -O2 -w $GI -c "$f" -o "$B/gumbo_$(basename "$f" .c).o" &
        while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do wait -n; done
    done
    for f in "$LH"/src/*.cpp; do
        g++ -O2 -std=c++17 -w -I"$LH/include" -I"$LH/include/litehtml" $GI -c "$f" -o "$B/lh_$(basename "$f" .cpp).o" &
        while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do wait -n; done
    done
    wait
    ar rcs "$CACHE/liblitehtml.a" "$B"/*.o
    echo "$LITEHTML_BUILD" > "$CACHE/litehtml.rev"
fi

O="$(mktemp -d)"
trap 'rm -rf "$O"' EXIT
CFLAGS="-std=c11 -O1 -w -D_GNU_SOURCE -DUI_DIR=\"$ROOT/ui\" -DDEVUI_BACKEND_PORT=$PORT -I$ROOT -I$ROOT/include -I$ROOT/third_party/stb -I$FT/include -pthread"
# BACKEND=ubus builds the preview on src/data_ubus.c (ubus and UCI) instead of the datad client
DATA_SRC=data; BIN=preview-host
if [ "${BACKEND:-datad}" = ubus ]; then DATA_SRC=data_ubus; BIN=preview-host-ubus; CFLAGS="$CFLAGS -DDEVUI_PLAIN_OPENWRT"; fi
for f in $DATA_SRC json touch_input key_input backlight devui_ext drm_disp livewp selfcheck; do
    gcc $CFLAGS -c "$ROOT/src/$f.c" -o "$O/$f.o"
done
gcc $CFLAGS -c "$ROOT/tests/ui/preview_host.c" -o "$O/preview_host.o"
g++ -std=c++17 -O1 -w -I"$ROOT/include" -I"$FT/include" -I"$LH/include" -I"$LH/include/litehtml" \
    -I"$ROOT/third_party/stb" -c "$ROOT/src/html_view.cpp" -o "$O/html_view.o"
g++ "$O"/*.o "$CACHE/liblitehtml.a" "$CACHE/libfreetype.a" -pthread -lm -o "$CACHE/$BIN"
echo "PREVIEW-BUILT $CACHE/$BIN (port $PORT)"
