#!/bin/sh
# Stand-in for the ubus command in the tests of src/data_ubus.c (DEVUI_UBUS):
# answers `ubus -S -t N call <object> <method> [args]` from canned replies and
# appends "object method args" to $MOCK_LOG.  An unknown object fails with
# status 4, as ubus does.  A file $MOCK_STATE.nocell takes the modem service away.
# A file $MOCK_STATE.device switches Wi-Fi to what the U60 Pro itself answers:
# fixtures/network.wireless.status.device.json is `ubus call network.wireless
# status` of the device (netifd over the qcacld driver, one 2.4 GHz access point,
# "mode" twice in the interface's config, name and key replaced), with no station.
# A file $MOCK_STATE.ownwifi puts the device's own wireless service on the bus
# (u60pro.wifi): fixtures/u60pro.wifi.status.device.json is its status on the
# device (one radio on 6 GHz at 320 MHz, the other off; name and key replaced);
# with $MOCK_STATE.ownwifi2 as well it answers with two radios (5 GHz + 6 GHz).
while [ $# -gt 0 ] && [ "$1" != call ]; do shift; done
[ "$1" = call ] || exit 1
obj=$2; method=$3; args=$4
[ -n "$MOCK_LOG" ] && printf '%s %s %s\n' "$obj" "$method" "$args" >> "$MOCK_LOG"
n=0
if [ -n "$MOCK_STATE" ]; then
	n=$(cat "$MOCK_STATE" 2>/dev/null || echo 0)
fi
case "$obj $method" in
"u60pro.hw status")
	cat <<'EOF'
{"model":"MU5250","battery":{"available":true,"present":true,"capacity":62,"status":"Charging","health":"Good","temp_c":31.2,"voltage_mv":3912,"current_ma":412,"power_mw":1612},"charger":{"available":true,"online":true,"type":"USB_PD","voltage_mv":8900,"current_ma":1850,"power_mw":16465},"charge":{"mode":"normal","stop":80,"start":70,"policy_available":true,"policy":0,"paused":false,"holding":false},"powerbank":{"available":true,"enabled":false},"screen":{"available":true,"on":true,"brightness":70,"timeout":60},"thermal":{"soc_c":41.2,"zones":[{"type":"cpuss-0","temp_c":41.2}]}}
EOF
	;;
"u60pro.hw reload") echo '{"ok":true}';;
"u60pro.hw powerbank") echo '{"ok":true,"powerbank":{"available":true,"enabled":true}}';;
"u60pro.usb status")
	echo '{"mode":"rndis","host":"windows","win":"rndis","other":"ecm","configured":"rndis","modes":["ncm","rndis","ecm"],"accel":["rndis","ecm"],"ipa":true,"netdev":"rndis0","usb0":"up","speed":"super-speed","udc":"configured","switching":false}';;
"u60pro.usb set") echo '{"ok":true}';;
"u60pro.slot status")
	echo '{"slot":"a","from_slot":true,"build":"u60-12e2d215245","slot_build":"u60-12e2d215245","trial":false,"trial_slot":"a","committed_slot":"a","failed_attempts":0,"max_attempts":7,"autocommit":false}';;
"system board")
	echo '{"kernel":"5.15.185-perf","hostname":"U60Pro","model":"ZTE U60 Pro (MU5250)","release":{"distribution":"OpenWrt","version":"25.12.6","description":"OpenWrt 25.12.6 r33311"}}';;
"system info")
	echo '{"localtime":1794293848,"uptime":93784,"load":[12000,9000,7000],"memory":{"total":1700000000,"free":900000000,"available":1190000000}}';;
"system reboot") echo '{}';;
"network.interface dump")
	cat <<'EOF'
{"interface":[{"interface":"loopback","up":true,"proto":"static","device":"lo","route":[]},{"interface":"lan","up":true,"proto":"static","device":"br-lan","metric":0,"ipv4-address":[{"address":"192.168.25.1","mask":24}],"route":[]},{"interface":"wan6","up":true,"proto":"dhcpv6","device":"rmnet_data0","metric":5,"route":[{"target":"::","mask":0}]},{"interface":"wan","up":true,"proto":"qmi","device":"wwan0","l3_device":"rmnet_data0","metric":10,"ipv4-address":[{"address":"10.87.21.164","mask":29}],"route":[{"target":"0.0.0.0","mask":0,"nexthop":"10.87.21.165"}],"data":{"up":false}}]}
EOF
	;;
"network.device status")
	[ -n "$MOCK_STATE" ] && echo $((n + 1)) > "$MOCK_STATE"
	echo "{\"up\":true,\"statistics\":{\"rx_bytes\":$((5000000000 + n * 3000000)),\"tx_bytes\":$((600000000 + n * 400000))}}";;
"network.wireless status")
	if [ -n "$MOCK_STATE" ] && [ -e "$MOCK_STATE.device" ]; then
		cat "$(dirname "$0")/fixtures/network.wireless.status.device.json"
		exit 0
	fi
	cat <<'EOF'
{"radio0":{"up":false,"disabled":false,"config":{"band":"2g","channel":"6"},"interfaces":[{"section":"default_radio0","ifname":"wlan0","config":{"mode":"ap","ssid":"U60Pro","encryption":"psk2","key":"key-2g"}}]},"radio1":{"up":true,"disabled":false,"config":{"band":"5g","channel":"36"},"interfaces":[{"section":"sta","ifname":"wlan1-sta","config":{"mode":"sta","ssid":"upstream"}},{"section":"default_radio1","ifname":"wlan1","config":{"mode":"ap","ssid":"U60Pro \"5G\"","encryption":"sae-mixed","key":"key-5g"}}]}}
EOF
	;;
"network.wireless up"|"network.wireless down") echo '{}';;
"iwinfo assoclist")
	[ -n "$MOCK_STATE" ] && [ -e "$MOCK_STATE.device" ] && { echo '{"results":[]}'; exit 0; }
	echo '{"results":[{"mac":"02:11:22:33:44:55","signal":-48},{"mac":"02:11:22:33:44:56","signal":-61}]}';;
"u60pro.wifi status")
	[ -n "$MOCK_STATE" ] && [ -e "$MOCK_STATE.ownwifi" ] || exit 4
	if [ -e "$MOCK_STATE.ownwifi2" ]; then
		cat "$(dirname "$0")/fixtures/u60pro.wifi.status.dual.json"
	else
		cat "$(dirname "$0")/fixtures/u60pro.wifi.status.device.json"
	fi
	;;
"u60pro.wifi set"|"u60pro.wifi radio")
	[ -n "$MOCK_STATE" ] && [ -e "$MOCK_STATE.ownwifi" ] || exit 4
	case "$args" in
	*radio9*) echo '{"ok":false,"error":"no such radio"}';;
	*) echo '{"ok":true}';;
	esac
	;;
"u60pro.cell status")
	[ -n "$MOCK_STATE" ] && [ -e "$MOCK_STATE.nocell" ] && exit 4
	echo '{"connected":true,"net_select":"nr","net_modes":["auto","nr","lte"],"operator":"China Mobile","mcc":460,"mnc":0,"rat":"5G","mode":"SA","bars":4,"band":"n78","imei":"860000000000000","qci":9,"ambr_dl":3000,"ambr_ul":200,"nr":{"band":"n78","rsrp":-88,"rsrq":-11,"rssi":-60,"sinr":17.5000000000000036,"pci":428,"cell_id":123456789,"arfcn":627264,"bw":"100MHz"},"lte":{"band":"B3","rsrp":-93,"rsrq":-9,"rssi":-65,"sinr":12.0}}';;
"u60pro.cell control") echo '{"ok":true,"result":{"mode":"sa"}}';;
*) exit 4;;
esac
