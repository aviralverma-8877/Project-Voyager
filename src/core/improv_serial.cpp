// Improv Serial handler (owner: system-core-engineer): WiFi setup over USB serial
// from the web flasher (ESP Web Tools shows "Connect to Wi-Fi" when the firmware
// answers Improv). Framing lives in improv.h (unit-tested); WiFi work is done by
// wifi_improv_provision / wifi_improv_scan (wifi-network-engineer).
//
// Active only while the LoRa-serial bridge is off: serial_to_lora feeds every USB
// byte here instead of discarding it. Long operations (connect, scan) run on their
// own task so the serial reader never blocks.
#include "improv_serial.h"
#include "improv.h"
#include "support_method.h"
#include <wifi/wifi_support.h>
#include <lora/lora_support.h>
#include "net_serial.h"

#define IMPROV_CONNECT_TIMEOUT_MS 20000
#define IMPROV_SCAN_MAX           20

static improv::Parser s_parser;
static volatile bool s_busy = false;   // a connect or scan task is running

// One Serial.write per packet: HardwareSerial writes are atomic per call, so debug
// lines from other tasks can only land between packets, which the browser skips.
static void send(const uint8_t* buf, size_t n)
{
    if (n == 0 || lora_serial) return;
    Serial.write(buf, n);
}

static void send_state(uint8_t state)
{
    uint8_t b[16];
    send(b, improv::build_state(state, b, sizeof b));
}

static void send_error(uint8_t err)
{
    uint8_t b[16];
    send(b, improv::build_error(err, b, sizeof b));
}

static void send_result(uint8_t cmd, const char* const* strs, size_t n)
{
    uint8_t b[improv::MAX_PACKET];
    send(b, improv::build_rpc_result(cmd, strs, n, b, sizeof b));
}

// Current state; when connected also the URL (as the spec asks for GET_CURRENT_STATE).
static void send_current_state()
{
    String url = wifi_sta_url();
    if (url.length() == 0)
    {
        send_state(improv::STATE_READY);
        return;
    }
    send_state(improv::STATE_PROVISIONED);
    const char* s[] = {url.c_str()};
    send_result(improv::CMD_GET_CURRENT_STATE, s, 1);
}

struct ProvisionArgs { char ssid[33]; char pass[65]; };

static void provision_task(void* param)
{
    ProvisionArgs* a = (ProvisionArgs*)param;
    String url;
    bool ok = wifi_improv_provision(a->ssid, a->pass, IMPROV_CONNECT_TIMEOUT_MS, url);
    memset(a->pass, 0, sizeof a->pass);
    delete a;
    if (ok)
    {
        send_state(improv::STATE_PROVISIONED);
        const char* s[] = {url.c_str()};
        send_result(improv::CMD_WIFI_SETTINGS, s, 1);
    }
    else
    {
        send_error(improv::ERROR_UNABLE_TO_CONNECT);
        send_state(improv::STATE_READY);
    }
    s_busy = false;
    vTaskDelete(NULL);
}

static void scan_task(void* param)
{
    WifiNet* nets = new (std::nothrow) WifiNet[IMPROV_SCAN_MAX];
    size_t n = nets ? wifi_improv_scan(nets, IMPROV_SCAN_MAX) : 0;
    for (size_t i = 0; i < n; i++)
    {
        char rssi[8];
        snprintf(rssi, sizeof rssi, "%d", (int)nets[i].rssi);
        const char* s[] = {nets[i].ssid, rssi, nets[i].secure ? "YES" : "NO"};
        send_result(improv::CMD_GET_WIFI_NETWORKS, s, 3);
    }
    send_result(improv::CMD_GET_WIFI_NETWORKS, nullptr, 0);   // end of list
    delete[] nets;
    s_busy = false;
    vTaskDelete(NULL);
}

static bool start_task(TaskFunction_t fn, const char* name, void* arg)
{
    s_busy = true;
    if (xTaskCreatePinnedToCore(fn, name, 6144, arg, 1, NULL, 1) == pdPASS) return true;
    s_busy = false;
    return false;
}

static void handle_rpc(const improv::Packet& p)
{
    uint8_t cmd;
    const uint8_t* payload;
    size_t plen;
    if (!improv::rpc_command(p, cmd, payload, plen))
    {
        send_error(improv::ERROR_INVALID_RPC);
        return;
    }
    switch (cmd)
    {
    case improv::CMD_GET_CURRENT_STATE:
        send_current_state();
        return;
    case improv::CMD_GET_DEVICE_INFO:
    {
        String name = hostname.length() ? hostname : String("Project Voyager");
        const char* s[] = {"Project Voyager", __DATE__, "ESP32", name.c_str()};
        send_result(improv::CMD_GET_DEVICE_INFO, s, 4);
        return;
    }
    case improv::CMD_WIFI_SETTINGS:
    {
        if (s_busy) { send_error(improv::ERROR_UNKNOWN); return; }
        ProvisionArgs* a = new (std::nothrow) ProvisionArgs();
        if (!a || !improv::parse_wifi_settings(payload, plen, a->ssid, a->pass))
        {
            delete a;
            send_error(improv::ERROR_INVALID_RPC);
            return;
        }
        send_error(improv::ERROR_NONE);
        send_state(improv::STATE_PROVISIONING);
        if (!start_task(provision_task, "improv_wifi", a))
        {
            delete a;
            send_error(improv::ERROR_UNKNOWN);
            send_state(improv::STATE_READY);
        }
        return;
    }
    case improv::CMD_VOYAGER_NET_START:
    {
        if (lora_serial || s_busy) { send_error(improv::ERROR_UNKNOWN); return; }
        uint16_t node = lora_node_id();
        char ip[16], id[8];
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", lora_proto::NET_PREFIX_A, lora_proto::NET_PREFIX_B,
                 node >> 8, node & 0xFF);
        snprintf(id, sizeof id, "%04X", node);
        // Airtime of a full and of a minimal NET frame, so the PC paces what it hands
        // us to what the radio can send (a flooded TX queue just drops everything).
        char full[12], small[12];
        snprintf(full, sizeof full, "%lu", (unsigned long)lora_frame_airtime_ms(lora_proto::MAX_PAYLOAD));
        snprintf(small, sizeof small, "%lu", (unsigned long)lora_frame_airtime_ms(lora_proto::NET_HDR + 20));
        const char* s[] = {ip, id, improv::VOYAGER_NET_PROTO, full, small};
        // Answer first (still Improv framing), then the port switches to SLIP.
        send_result(improv::CMD_VOYAGER_NET_START, s, 5);
        Serial.flush();
        if (!net_enter()) send_error(improv::ERROR_UNKNOWN);
        return;
    }
    case improv::CMD_GET_WIFI_NETWORKS:
        if (s_busy || !start_task(scan_task, "improv_scan", nullptr))
            send_result(improv::CMD_GET_WIFI_NETWORKS, nullptr, 0);   // empty list, not an error
        return;
    default:
        send_error(improv::ERROR_UNKNOWN_RPC);
        return;
    }
}

void improv_feed(uint8_t b)
{
    improv::Packet p;
    int r = s_parser.feed(b, p);
    if (r < 0)
        send_error(improv::ERROR_INVALID_RPC);
    else if (r > 0 && p.type == improv::TYPE_RPC)
        handle_rpc(p);
}

void improv_reset()
{
    s_parser.reset();
}
