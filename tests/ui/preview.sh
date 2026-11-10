#!/bin/bash
# Render every DevUI page on a Linux host into PNGs plus a contact sheet.
# usage: bash tests/ui/preview.sh OUT_DIR [theme: 0 dark, 1 light] [state.json]
# Needs gcc/g++, git, python3 with Pillow, and a CJK font (DEVUI_FONT, default
# Noto Sans CJK). The device font differs, so line breaks can shift slightly.
# DEVUI_PREVIEW_WALLPAPER=DIR renders with DIR/wallpaper.jpg and wallpaper.conf.
# BACKEND=ubus renders with the ubus backend from the system's own ubus and uci (or the programs named by
# DEVUI_UBUS and DEVUI_UCI) instead of a mock datad.
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
THEME="${2:-0}"
STATE="${3:-$ROOT/tests/ui/sample_state.json}"
PORT="${DEVUI_PREVIEW_PORT:-29462}"
CACHE="${DEVUI_PREVIEW_CACHE:-$HOME/.cache/devui-preview}"
for f in /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc \
         /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf; do
    [ -n "${DEVUI_FONT:-}" ] || { [ -f "$f" ] && export DEVUI_FONT="$f"; }
done

bash "$ROOT/tests/ui/build_preview.sh" "$PORT" >/dev/null

rm -f "$OUT"/*.ppm "$OUT"/*.png
if [ "${BACKEND:-datad}" = ubus ]; then
    DEVUI_UBUS_WARMUP_MS="${DEVUI_UBUS_WARMUP_MS:-1200}" "$CACHE/preview-host-ubus" "$OUT" "$THEME"
else
    LOG="$(mktemp)"
    python3 "$ROOT/tests/mock_datad.py" "$PORT" "$LOG" "$STATE" &
    MOCK=$!
    trap 'kill $MOCK 2>/dev/null; wait $MOCK 2>/dev/null || true; rm -f "$LOG"' EXIT
    for _ in $(seq 50); do
        python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), 1)" "$PORT" 2>/dev/null && break
        sleep 0.1
    done
    "$CACHE/preview-host" "$OUT" "$THEME" 2>/dev/null
fi

python3 - "$OUT" <<'PY'
import glob, os, sys
from PIL import Image, ImageDraw
out = sys.argv[1]
names = sorted(os.path.basename(p)[:-4] for p in glob.glob(out + "/*.ppm"))
for n in names:
    Image.open(f"{out}/{n}.ppm").save(f"{out}/{n}.png")
    os.remove(f"{out}/{n}.ppm")
cols = 6
rows = (len(names) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 330, rows * 500), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, n in enumerate(names):
    x, y = (i % cols) * 330 + 5, (i // cols) * 500 + 4
    sheet.paste(Image.open(f"{out}/{n}.png"), (x, y))
    d.text((x, y + 482), n, fill=(220, 220, 220))
sheet.save(f"{out}/sheet.png")
print(f"{len(names)} screens -> {out}/sheet.png")
PY
