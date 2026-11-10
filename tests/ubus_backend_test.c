/*
 * ubus_backend_test.c - src/data_ubus.c against tests/mock_ubus.sh and mock_uci.sh.
 *
 * tests/run_host_tests.sh builds and runs it with DEVUI_UBUS, DEVUI_UCI,
 * DEVUI_DHCP_LEASES, MOCK_LOG, MOCK_STATE and MOCK_UCI set.
 *
 * SPDX-License-Identifier: MIT
 */
#include "data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b))) { fails++; fprintf(stderr, "FAIL %s:%d: %s is \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); } } while (0)

static int logged(const char *needle)
{
    const char *path = getenv("MOCK_LOG");
    char line[512];
    FILE *fp = path ? fopen(path, "r") : NULL;
    int found = 0;
    if (!fp) return 0;
    while (fgets(line, sizeof line, fp))
        if (strstr(line, needle)) found = 1;
    fclose(fp);
    return found;
}

int main(void)
{
    static devui_data_t d;
    char reply[1024];
    int b = 0, t = 0;

    setenv("DEVUI_UBUS_WARMUP_MS", "300", 1);
    CHECK(data_backend_init() == 1);
    CHECK(data_backend_poll(0) == 1);
    CHECK(data_backend_commit_latest() == 1);
    CHECK(data_refresh(&d) == 1);
    CHECK(d.valid == 1);
    CHECK_STR(data_model_tag(), "MU5250");

    /* battery and charger: u60pro.hw */
    CHECK(d.bat_percent == 62);
    CHECK(d.bat_temp == 31);
    CHECK(d.charging == 1);
    CHECK(d.charger_connect == 1);
    CHECK(d.bat_uv == 3912000 && d.bat_ua == 412000);
    CHECK(d.chg_uv == 8900000 && d.chg_ua == 1850000);
    CHECK(d.dps == 0);
    CHECK(d.cpu_temp == 41);

    /* system: procd */
    CHECK_STR(d.model, "ZTE U60 Pro (MU5250)");
    CHECK_STR(d.fw, "OpenWrt 25.12.6 r33311");
    CHECK(d.uptime == 93784);
    CHECK(d.mem_total == 1700000000L && d.mem_avail == 1190000000L && d.mem_used_pct == 30);

    /* the uplink is the IPv4 default route (not the IPv6 one of a lower metric), counted on its layer 3 device */
    CHECK_STR(d.wan_status, "connected");
    CHECK(logged("network.device status {\"name\":\"rmnet_data0\"}"));
    CHECK(d.rx_bytes >= 5000000000L);
    CHECK(d.rx_speed > 0 && d.tx_speed > 0);
    CHECK_STR(d.dhcp_ip, "192.168.25.1");
    CHECK_STR(d.dhcp_start, "192.168.25.100");
    CHECK_STR(d.dhcp_limit, "150");
    CHECK_STR(d.dhcp_leasetime, "43200");   /* 12h in UCI */

    /* Wi-Fi: the access point of the radio that is up, not the station on it */
    CHECK(d.wifi_enabled == 1);
    CHECK_STR(d.wifi_ssid, "U60Pro \"5G\"");
    CHECK_STR(d.wifi_key, "key-5g");
    CHECK_STR(d.wifi_enc, "sae-mixed");
    CHECK(logged("iwinfo assoclist {\"device\":\"wlan1\"}"));
    CHECK(!logged("\"device\":\"wlan0\""));

    /* clients: the leases file and the stations */
    CHECK(d.clients_total == 3 && d.clients_wifi == 2 && d.clients_lan == 1);
    CHECK(d.client_n == 3);
    CHECK_STR(d.client[0].name, "phone");
    CHECK_STR(d.client[0].ip, "192.168.25.120");
    CHECK_STR(d.client[2].name, "");

    /* cellular: u60pro.cell */
    CHECK_STR(d.net_type, "5G SA");
    CHECK_STR(d.operator_name, "China Mobile");
    CHECK(d.bars == 4 && d.mcc == 460 && d.mnc == 0);
    CHECK_STR(d.nr_band, "n78");
    CHECK(d.nr_rsrp == -88 && d.nr_rsrq == -11 && d.nr_pci == 428 && d.nr_channel == 627264);
    CHECK_STR(d.nr_snr, "17.5");
    CHECK_STR(d.nr_bw, "100MHz");
    CHECK(d.lte_rsrp == -93);
    CHECK_STR(d.lte_snr, "12.0");
    CHECK(d.qci == 9 && d.ambr_dl == 3000.0);
    CHECK_STR(d.imei, "860000000000000");

    /* what this system does not have */
    CHECK(d.als_enabled == -1 && d.neighbor_enabled == -1 && d.modem_n == 0 && !d.agg_present && !d.cool_present);
    CHECK(datad_request("GET", "/modem/signal-metrics", NULL, NULL, 0, 100) == 404);

    /* screen settings: UCI u60pro.screen */
    CHECK(data_screen_settings(&b, &t) == 1 && b == 70 && t == 60);
    data_screen_settings_save(70, 60);          /* unchanged: nothing is written */
    usleep(200000);
    CHECK(!logged("uci set u60pro.screen"));
    data_screen_settings_save(45, 120);
    usleep(400000);
    CHECK(logged("uci set u60pro.screen.brightness=45"));
    CHECK(logged("uci set u60pro.screen.timeout=120"));
    CHECK(logged("uci commit u60pro"));
    CHECK(data_screen_settings(&b, &t) == 1 && b == 45 && t == 120);

    /* queries in datad's shape */
    CHECK(datad_control("wifi.state", NULL, reply, sizeof reply, 800) == 1);
    CHECK(strstr(reply, "{\"band\":\"2g\",\"enabled\":false}") != NULL);
    CHECK(strstr(reply, "{\"band\":\"5g\",\"enabled\":true}") != NULL);
    CHECK(strstr(reply, "\"module\":{\"enabled\":true}") != NULL);
    CHECK(datad_control("wifi.status", NULL, reply, sizeof reply, 800) == 1);
    CHECK(strstr(reply, "\"main_2g\":{\"ssid\":\"U60Pro\",\"key\":\"key-2g\"}") != NULL);
    CHECK(strstr(reply, "\"main_5g\":{\"ssid\":\"U60Pro \\\"5G\\\"\",\"key\":\"key-5g\"}") != NULL);

    /* actions */
    CHECK(datad_control("wifi.set_module", "{\"enabled\":false}", NULL, 0, 800) == 1);
    CHECK(logged("network.wireless down \n"));
    CHECK(datad_control("wifi.radio.set", "{\"band\":\"2g\",\"enabled\":true}", NULL, 0, 800) == 1);
    CHECK(logged("network.wireless up {\"radio\":\"radio0\"}"));
    CHECK(datad_control("wifi.radio.set", "{\"band\":\"6g\",\"enabled\":true}", NULL, 0, 800) == 0);
    CHECK(datad_control("power.direct_supply.set", "{\"enabled\":true}", NULL, 0, 800) == 1);
    CHECK(logged("uci set u60pro.battery.charge_mode=bypass"));
    CHECK(logged("u60pro.hw reload"));
    CHECK(datad_control("device.reboot", NULL, NULL, 0, 800) == 1);
    CHECK(logged("system reboot"));
    CHECK(datad_control("network.set_mode", "{\"mode\":\"sa\"}", reply, sizeof reply, 800) == 1);
    CHECK(logged("u60pro.cell control {\"action\":\"network.set_mode\",\"params\":{\"mode\":\"sa\"}}"));
    CHECK(strstr(reply, "\"ok\":true") != NULL);
    CHECK(datad_control("state.set_interval", "{\"milliseconds\":500}", NULL, 0, 800) == 1);
    CHECK(datad_control("nfc.set", "{\"enabled\":true}", NULL, 0, 800) == 0);
    /* a vendor fallback command must never run here */
    datad_control_async("nfc.set", "{\"enabled\":true}", "touch ubus_backend_test.fallback");
    datad_ubus_call_async("system", "info", NULL, "touch ubus_backend_test.fallback");
    usleep(400000);
    CHECK(access("ubus_backend_test.fallback", F_OK) != 0);

    /* without a modem service the uplink interface stands for the operator */
    {
        char flag[512];
        FILE *fp;
        snprintf(flag, sizeof flag, "%s.nocell", getenv("MOCK_STATE"));
        fp = fopen(flag, "w");
        if (fp) fclose(fp);
    }
    usleep(900000);
    (void)data_backend_poll(0);
    CHECK(data_refresh_live(&d) == 1);
    CHECK_STR(d.operator_name, "WAN");
    CHECK_STR(d.net_type, "");
    CHECK_STR(d.wan_status, "connected");

    data_backend_close();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
