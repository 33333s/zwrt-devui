#!/bin/sh
# Stand-in for the uci command in the tests of src/data_ubus.c (DEVUI_UCI):
# `uci -q get <key>` answers from a few fixed values (and from what was set
# before, kept in $MOCK_UCI), `set` and `commit` are appended to $MOCK_LOG.
[ "$1" = -q ] && shift
case "$1" in
get)
	if [ -n "$MOCK_UCI" ] && grep -q "^$2=" "$MOCK_UCI" 2>/dev/null; then
		grep "^$2=" "$MOCK_UCI" | tail -1 | cut -d= -f2-
		exit 0
	fi
	case "$2" in
	dhcp.lan.start) echo 100;;
	dhcp.lan.limit) echo 150;;
	dhcp.lan.leasetime) echo 12h;;
	u60pro.screen.brightness) echo 70;;
	u60pro.screen.timeout) echo 60;;
	*) exit 1;;
	esac;;
set)
	[ -n "$MOCK_LOG" ] && echo "uci set $2" >> "$MOCK_LOG"
	[ -n "$MOCK_UCI" ] && echo "$2" >> "$MOCK_UCI"
	;;
commit)
	[ -n "$MOCK_LOG" ] && echo "uci commit $2" >> "$MOCK_LOG"
	;;
*) exit 1;;
esac
