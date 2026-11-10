#!/bin/sh
# Host-side tests: build the datad client natively and run it against
# tests/mock_datad.py, then check what the mock received; then the ubus backend
# against tests/mock_ubus.sh and tests/mock_uci.sh.
# usage: sh tests/run_host_tests.sh   (needs cc and python3)
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${DEVUI_TEST_PORT:-29461}
WORK=$(mktemp -d)
MOCK_PID=
cleanup() {
    if [ -n "$MOCK_PID" ]; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

${CC:-cc} -std=c11 -O1 -Wall -Wextra -Wno-unused-parameter -Werror -D_GNU_SOURCE \
    -DDEVUI_BACKEND_PORT="$PORT" -I"$ROOT/include" -pthread \
    "$ROOT/src/data.c" "$ROOT/src/json.c" "$ROOT/tests/datad_client_test.c" \
    -o "$WORK/datad_client_test"

python3 "$ROOT/tests/mock_datad.py" "$PORT" "$WORK/calls.log" &
MOCK_PID=$!
i=0
until python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), 1)" "$PORT" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { echo "mock datad did not start" >&2; exit 1; }
    sleep 0.1
done

(cd "$WORK" && ./datad_client_test)

python3 - "$WORK/calls.log" <<'PY'
import json, sys
calls = [tuple(json.loads(line)) for line in open(sys.argv[1])]
want = [
    ("/control", {"action": "wifi.status", "params": {}}),
    ("/control", {"action": "wifi.radio.set", "params": {"band": "2g", "enabled": False}}),
    ("/control", {"action": "qos.reload", "params": {}}),
    ("/control", {"action": "wifi.radio.set", "params": {"band": "5g", "enabled": True}}),
    ("/ubus/call", {"service": "zte_nwinfo_api", "method": "nwinfo_reset_band_cell_setting", "args": {}}),
]
missing = [w for w in want if w not in calls]
if missing:
    sys.exit("mock datad did not receive: %r\ngot: %r" % (missing, calls))
print("mock datad: received every expected request")
PY

# The ubus backend (plain OpenWrt): src/data_ubus.c against stand-ins for the
# ubus and uci commands.
${CC:-cc} -std=c11 -O1 -Wall -Wextra -Wno-unused-parameter -Werror -D_GNU_SOURCE     -I"$ROOT/include" -pthread     "$ROOT/src/data_ubus.c" "$ROOT/tests/ubus_backend_test.c"     -o "$WORK/ubus_backend_test"
printf '%s
' "$(($(date +%s) + 3600)) 02:11:22:33:44:55 192.168.25.120 phone *"     "$(($(date +%s) + 3600)) 02:11:22:33:44:56 192.168.25.121 laptop 01:02:11:22:33:44:56"     "$(($(date +%s) + 3600)) 02:11:22:33:44:57 192.168.25.122 * *" > "$WORK/dhcp.leases"
: > "$WORK/ubus.log"
(cd "$WORK" && DEVUI_UBUS="$ROOT/tests/mock_ubus.sh" DEVUI_UCI="$ROOT/tests/mock_uci.sh"     DEVUI_DHCP_LEASES="$WORK/dhcp.leases" MOCK_LOG="$WORK/ubus.log" MOCK_STATE="$WORK/ubus.state"     MOCK_UCI="$WORK/uci.set" ./ubus_backend_test)
