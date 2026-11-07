#!/bin/sh
# Boot launcher for the DevUI screen UI (U60 Pro / TopFlow). Installed at
# /data/plugins/zwrt-devui/start.sh
# and invoked either by the procd init service or by the legacy rc.local hook.
# In procd mode it execs the foreground UI process. In legacy mode it keeps the
# old nohup-based launch path for manual installs and older plugins.
#
# SPDX-License-Identifier: MIT
DEVUI_DIR=/data/plugins/zwrt-devui
UI_DIR=$DEVUI_DIR/ui
LEGACY_UI_DIR=/data/ui
# Before the rename to zwrt-devui the plugin installed here as u60pro-devui.
OLD_DEVUI_DIR=/data/plugins/u60pro-devui
DEVUI_BIN=$DEVUI_DIR/zwrt-devui
# zwrt-datad is installed and managed by its own installer under
# /data/zwrt-datad; its service.sh owns the flags, token, log and locking.
DATAD_SERVICE=${DATAD_SERVICE:-/data/zwrt-datad/service.sh}
TOUCH_KO=/lib/modules/sitronix-ts-spi.ko
MODE="${1:-legacy}"

mkdir -p "$DEVUI_DIR" "$UI_DIR"

count_ui_pages() {
    find "$1" -maxdepth 1 -type f -name '*.html' 2>/dev/null | wc -l | tr -d ' '
}

migrate_legacy_ui() {
    [ -d "$LEGACY_UI_DIR" ] || return 0
    [ -f "$LEGACY_UI_DIR/.lockpin" ] && [ ! -f "$UI_DIR/.lockpin" ] \
        && cp -af "$LEGACY_UI_DIR/.lockpin" "$UI_DIR/.lockpin" 2>/dev/null
    old_count=$(count_ui_pages "$LEGACY_UI_DIR")
    new_count=$(count_ui_pages "$UI_DIR")
    if [ "$new_count" -le 0 ] && [ "$old_count" -gt 0 ]; then
        cp -af "$LEGACY_UI_DIR"/. "$UI_DIR"/ 2>/dev/null || true
    fi
}

# Carry the settings, PIN and model cache over from the pre-rename install. The
# old directory itself is left for the plugin to remove.
migrate_old_install() {
    [ -d "$OLD_DEVUI_DIR" ] || return 0
    for f in devui.conf .device signal.cache ui/.lockpin; do
        [ -f "$OLD_DEVUI_DIR/$f" ] && [ ! -f "$DEVUI_DIR/$f" ] \
            && cp -af "$OLD_DEVUI_DIR/$f" "$DEVUI_DIR/$f" 2>/dev/null
    done
}

read_mode_main_state() {
    awk -F"'" '/option mode_main_state/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null
}

read_reboot_reason_code() {
    awk -F"'" '/option reboot_reason_code/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null
}

boot_trace() {
    LOG="$DEVUI_DIR/boot-trace.log"
    {
        echo "=== $(date '+%Y-%m-%d %H:%M:%S') ==="
        echo "mode_main_state=$mode_main_state"
        echo "reboot_reason_code=$reboot_reason_code"
        echo "bootmode=$BOOTMODE"
        echo -n "cmdline="
        cat /proc/cmdline 2>/dev/null
        for f in \
            /sys/class/power_supply/usb/online \
            /sys/class/power_supply/usb/voltage_now \
            /sys/class/power_supply/battery/status \
            /sys/class/power_supply/battery/capacity \
            /sys/class/power_supply/charger_zte/present_mbb \
            /sys/class/power_supply/charger_zte/status_mbb \
            /sys/class/power_supply/type-c_zte/present_mbb \
            /sys/class/power_supply/type-c_zte/real_type_mbb \
            /sys/class/power_supply/statistics_zte/batt_status \
            /sys/class/power_supply/statistics_zte/batt_online \
            /sys/class/power_supply/battery_zte/status_mbb \
            /sys/class/power_supply/battery_zte/online_mbb
        do
            [ -e "$f" ] && echo "$f=$(cat "$f" 2>/dev/null)"
        done
        for e in /dev/input/event*; do
            [ -e "$e" ] || continue
            echo "$e=$(cat /sys/class/input/${e##*/}/device/name 2>/dev/null)"
        done
        echo
    } >> "$LOG"
    tail -n 160 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
}

stop_vendor_ui() {
    # Drop the vendor screen program's procd instance rather than running its
    # init script's stop: on TopFlow (MU5252) that stop_service also unloads
    # the touch driver (rmmod sitronix_ts_spi), and loading it straight back is
    # unreliable. Deleting the instance also keeps procd from respawning it.
    ubus call service delete '{"name":"zte_topsw_devui"}' >/dev/null 2>&1
    # It can take a few seconds to exit on SIGTERM (procd allows 5 s). Wait for
    # it, kill what is left, and give DRM a moment to drop the old master, or
    # our SETCRTC fails with EACCES.
    i=0
    while pidof zte_topsw_devui >/dev/null 2>&1 && [ "$i" -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    if pidof zte_topsw_devui >/dev/null 2>&1; then
        killall -9 zte_topsw_devui 2>/dev/null
        i=0
        while pidof zte_topsw_devui >/dev/null 2>&1 && [ "$i" -lt 20 ]; do
            sleep 0.1
            i=$((i + 1))
        done
    fi
    sleep 0.5
    # a pre-rename u60pro-devui still holding the screen would block ours
    killall u60pro-devui 2>/dev/null
    # TopFlow: if an earlier stop already unloaded the touch driver, load it
    # back. Right after an unload the controller can still read chip ID 0 and
    # the probe fails, so wait a few seconds and unload a failed probe first.
    [ -f "$TOUCH_KO" ] || return 0
    tries=0
    while ! grep -q 'sitronix_ts' /proc/bus/input/devices 2>/dev/null && [ "$tries" -lt 3 ]; do
        tries=$((tries + 1))
        rmmod sitronix_ts_spi 2>/dev/null
        sleep 3
        insmod "$TOUCH_KO" 2>/dev/null
        sleep 1
    done
}

# Legacy mode has no procd to respawn the UI, so run it again here after a
# crash or a hang it caught itself (exit 70, see src/selfcheck.c). Not after a
# stop: a clean exit (SIGTERM) or SIGKILL ends this loop too. If it keeps
# failing, give the screen back to the vendor UI.
supervise() {
    fails=0
    since=$(cut -d. -f1 /proc/uptime)
    while :; do
        "$DEVUI_BIN"
        rc=$?
        case "$rc" in
            70|132|134|135|136|139) ;;   # hang, SIGILL, SIGABRT, SIGBUS, SIGFPE, SIGSEGV
            *) return 0 ;;
        esac
        now=$(cut -d. -f1 /proc/uptime)
        if [ $((now - since)) -gt 600 ]; then fails=0; since=$now; fi
        fails=$((fails + 1))
        echo "=== $(date '+%Y-%m-%d %H:%M:%S') exit $rc, start #$fails" >> "$DEVUI_DIR/selfcheck.log"
        if [ "$fails" -gt 5 ]; then
            echo "=== too many in 10 min, vendor UI back" >> "$DEVUI_DIR/selfcheck.log"
            /etc/init.d/zte_topsw_devui enable 2>/dev/null
            /etc/init.d/zte_topsw_devui start 2>/dev/null
            return 1
        fi
        sleep 1
    done
}

start_datad() {
    [ "$BOOTMODE" = normal ] || return 0
    if [ ! -f "$DATAD_SERVICE" ]; then
        echo "zwrt-datad not installed ($DATAD_SERVICE missing); UI runs without backend data" >&2
        return 0
    fi
    # Idempotent and serialized by service.sh's own lock, so racing the
    # rc.local entry the datad installer writes cannot start a second daemon.
    sh "$DATAD_SERVICE" start
}

# Power-off charging boots do not expose silent_boot.mode=nonsilent. We still
# start DevUI there now, but only in its own full-screen charging mode.
BOOTMODE=charge
mode_main_state="$(read_mode_main_state)"
reboot_reason_code="$(read_reboot_reason_code)"
case "$mode_main_state" in
    mode_power_off_*) BOOTMODE=charge ;;
    mode_power_on|mode_power_on_charger) BOOTMODE=normal ;;
    *)
        if grep -q 'silent_boot.mode=nonsilent' /proc/cmdline 2>/dev/null; then
            BOOTMODE=normal
        fi
        ;;
esac
boot_trace
migrate_old_install
migrate_legacy_ui

case "$MODE" in
    procd)
        stop_vendor_ui
        [ -x "$DEVUI_BIN" ] || exit 1
        exec "$DEVUI_BIN"
        ;;
    legacy)
        stop_vendor_ui
        # Data aggregator first on normal boots (the UI reads its snapshot).
        # For charge-only boots the full-screen charging UI can fall back to
        # sysfs, so there is no need to wake extra polling daemons.
        start_datad
        [ -x "$DEVUI_BIN" ] && (trap '' HUP; supervise) >/tmp/zwrt-devui.log 2>&1 </dev/null &
        ;;
    *)
        echo "usage: $0 [procd|legacy]" >&2
        exit 2
        ;;
esac
