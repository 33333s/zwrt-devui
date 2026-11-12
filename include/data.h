/*
 * data.h - device state consumed from the backend.
 *
 * Two backends implement this interface: data.c reads the zwrt-datad HTTP/SSE
 * snapshot (vendor firmware), data_ubus.c reads ubus and UCI (plain OpenWrt).
 * The GUI itself never calls either. If the backend isn't running, refresh
 * returns 0 and the UI shows placeholders.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZWRT_DEVUI_DATA_H
#define ZWRT_DEVUI_DATA_H

#include <stddef.h>
#include <stdint.h>

#define DEVUI_SMS_MAX 32
#define DEVUI_SMS_TEXT_MAX 16384
#define DEVUI_NEIGHBOR_MAX 32

/* One neighbor cell; has_* is 0 where datad reported null. */
typedef struct {
    char rat[4];                  /* "LTE" / "NR" */
    int  pci, band, rsrp, rsrq;   /* band number, dBm, dB */
    long arfcn;
    unsigned char has_band, has_arfcn, has_rsrp, has_rsrq;
} devui_neighbor_t;

#define DEVUI_MODEM_MAX 4
#define DEVUI_AGG_PATH_MAX 6

/* One cellular modem from /state.modems[] (TopFlow: integrated 5G plus two
 * external 4G modems). Signal values are 0 / "" when datad left them out. */
typedef struct {
    char id[8];                   /* "5G", "4G1", "4G2" */
    char type[16], band[24], oper[32], sim_state[32], wwan[24];
    int  bars, lte_rsrp, nr_rsrp;
    char lte_snr[12], nr_snr[12];
    long rx_speed, tx_speed;
    int  temp, has_temp;          /* thermal.modems[] matched by id */
} devui_modem_t;

/* One aggregation (bonding) path from /state.aggregation.paths[]. */
typedef struct {
    char label[16];
    int  online;
    double latency_ms, loss_pct;
} devui_agg_path_t;

typedef struct {
    int  valid;

    /* datad model template ("MU5250", "MU5252", ...), "" when not reported */
    char model_tag[16];

    /* network */
    char net_type[16];
    int  bars;
    char operator_name[48];
    char band[16];
    char nr_band[16];
    int  nr_rsrp, nr_rsrq, nr_rssi;
    char nr_snr[12];
    int  lte_rsrp, lte_rsrq, lte_rssi;
    char lte_snr[12];
    int  rssi, mcc, mnc, nr_pci;
    long nr_cell_id, nr_channel;
    char nr_bw[12];
    char nrca[256], lteca[256], ltecasig[256];
    char wan_status[32];
    char net_select[16];
    int  hsr;
    char sa_bands[256], nsa_bands[256], lte_bands[256];

    /* battery */
    int  bat_percent, bat_temp, charger_connect;
    int  charging;   /* vendor charge_status: 1 charging, 2 discharging, 3 not charging, 4 full */
    long chg_uv, chg_ua, bat_uv, bat_ua;   /* charger/battery voltage(µV)/current(µA) */

    /* clients */
    int  clients_total, clients_wifi, clients_lan;
    struct { char name[40], ip[24], mac[20]; } client[16];
    int  client_n;

    /* sms (read-only) */
    int  sms_unread;
    struct { long id; char num[40], date[16], text[DEVUI_SMS_TEXT_MAX]; int unread; } sms[DEVUI_SMS_MAX];
    int  sms_n;

    /* wifi (main SSID) */
    char wifi_ssid[64], wifi_key[64], wifi_enc[24];
    int  wifi_enabled;

    /* nfc tap-to-share */
    int  nfc_switch;

    /* power.direct_supply: -1 when the block is absent or the mode is unknown */
    int  dps;

    /* ambient_light (TopFlow): sensor on/off and its raw level; -1 when the
     * block is absent, lux also -1 while the sensor is off or not yet read */
    int  als_enabled;
    long als_lux;

    /* neighbor: DIAG collector state plus the merged cell list
     * (neighbor.cells while the collector runs, else the vendor neighbor.lte scan).
     * neighbor_enabled is -1 when the block is absent. */
    int  neighbor_enabled;
    char neighbor_status[20];
    devui_neighbor_t neighbor[DEVUI_NEIGHBOR_MAX];
    int  neighbor_n;

    /* datad runtime: global sample/SSE period (0 if absent) and own version */
    int  sample_interval_ms;
    char datad_version[24];

    /* dhcp / lan */
    char dhcp_ip[24], dhcp_start[24], dhcp_limit[8], dhcp_leasetime[12];

    /* traffic (bytes, bytes/s) */
    long rx_speed, tx_speed, rx_bytes, tx_bytes;

    /* qos (parsed from modem key.log by the backend) */
    int    qci;
    double ambr_dl, ambr_ul;   /* Mbps */
    char   usb_mode[16];       /* "user" = adb off, "debug" = adb on */

    /* system */
    long uptime, cpu_temp, cpu_usage, mem_used_pct, mem_total, mem_avail;
    char model[64], fw[80], sw_version[80], imei[24];

    /* multi-modem devices (TopFlow); modem_n is 0 where datad has no modems[] */
    devui_modem_t modem[DEVUI_MODEM_MAX];
    int  modem_n;

    /* aggregation: agg_present is 0 when the block is absent */
    int  agg_present, agg_enabled, agg_online, agg_paths, agg_paths_online;
    char agg_state[20], agg_transport[12];
    devui_agg_path_t agg_path[DEVUI_AGG_PATH_MAX];
    int  agg_path_n;

    /* cooling: cool_present is 0 when the block is absent */
    int  cool_present;
    char fan_mode[16], liquid_mode[16];   /* automatic|custom|always_on / automatic|low|high */
    int  fan_pct, fan_temp, liquid_active, liquid_pct;

    /* plain OpenWrt (the ubus backend; 0 / "" from datad): the network function of the USB port
     * (u60pro.usb: "ncm", "rndis" or "ecm", the kind of host seen, a switch under way) and reverse
     * charging (u60pro.hw powerbank: usb_pb_present is 0 where the device has none) */
    int  usbnet_present, usbnet_busy;
    char usbnet_mode[8], usbnet_host[12];
    int  usb_pb_present, usb_pb_on;

    /* plain OpenWrt: the firmware updater (u60pro.ota). ota_state is "" where there is none, else idle, checking,
     * available, downloading, installing, installed, rebooting or error; ota_new names the update on offer and
     * ota_pending the build that is installed and waits for a restart ("" for none). */
    char ota_state[16], ota_new[48], ota_pending[32];
} devui_data_t;

/* Start the backend transport and seed the first visible snapshot if available. */
int  data_backend_init(void);

/* Drain SSE events and reconnect as needed. Returns 1 when the live snapshot changed. */
int  data_backend_poll(uint32_t now_ms);

/* Promote the latest live snapshot to the UI-visible snapshot. */
int  data_backend_commit_latest(void);

/* Bumped on every new SSE connection. datad keeps runtime switches such as
 * neighbor.set and state.set_interval only per process, so a change here means
 * they may need to be applied again. */
unsigned data_backend_generation(void);

/* Close the transport socket. Safe to call during shutdown. */
void data_backend_close(void);

/* datad device.api_template of the latest live snapshot, "" when unknown. */
const char *data_model_tag(void);

/* ambient_light.lux of the latest live snapshot, -1 when unknown; *enabled
 * gets ambient_light.enabled (-1 when the device has no sensor block). */
long data_ambient_light(int *enabled);

/* Copy the current UI-visible snapshot. Returns 1 on success. */
int data_refresh(devui_data_t *d);

/* Copy the latest live snapshot even if UI-visible refresh is paused. */
int data_refresh_live(devui_data_t *d);

/* One request to the local datad listener (127.0.0.1:9460, no token needed
 * there). Copies the response body into out (may be NULL) and returns the HTTP
 * status, or -1 when datad is unreachable or the reply is malformed. Blocking. */
int datad_request(const char *method, const char *path, const char *body,
                  char *out, size_t cap, int timeout_ms);

/* POST /control {"action":action,"params":params_json}. params_json is a JSON
 * object or NULL for {}. Returns 1 when datad answered ok:true. The full reply
 * (including "result") is copied into out when given. Blocking. */
int datad_control(const char *action, const char *params_json,
                  char *out, size_t cap, int timeout_ms);

/* Fire-and-forget datad_control on a detached thread. When the call fails and
 * fallback_cmd is non-NULL it is run with system() instead, so device actions
 * keep working while datad is down. */
void datad_control_async(const char *action, const char *params_json,
                         const char *fallback_cmd);

/* Fire-and-forget POST /ubus/call for vendor calls datad has no action for. */
void datad_ubus_call_async(const char *service, const char *method,
                           const char *args_json, const char *fallback_cmd);

/* Screen settings kept by the system instead of devui.conf (the ubus backend:
 * UCI u60pro.screen, shared with the web interface): the backlight in per cent
 * and the screen-off time in seconds, -1 for one that is not set. Returns 0
 * when the backend has no such store (datad) or has not read it yet. */
int data_screen_settings(int *brightness_pct, int *timeout_s);

/* Store them there (in the background); a no-op on a backend without the store. */
void data_screen_settings_save(int brightness_pct, int timeout_s);

#endif /* ZWRT_DEVUI_DATA_H */
