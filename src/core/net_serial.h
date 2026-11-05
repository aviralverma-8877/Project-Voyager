// LoRa network mode on the USB serial port (owner: system-core-engineer).
//
// Voyager Link (tools/voyager-link) turns it on with the Improv vendor command
// IMPROV_CMD_NET_START; from then on the USB link carries SLIP frames:
//   PC -> board: IPv4 packet (routed to its node over LoRa) or control frame
//   board -> PC: IPv4 packets received over LoRa for this node
// Control frames start with 0x00 (never the first byte of an IPv4 packet):
//   00 01 keepalive (board answers 00 81)    00 02 leave
//   board -> PC: 00 82 [queued] when the radio queue fills (PC slows its pacing)
//   00 03 [node u16] + IPv4: send to that node whatever the IP destination
//         (internet via a gateway node; protocol "2")
// The mode ends on "leave", or after NET_IDLE_TIMEOUT_MS without any frame from the
// PC (helper closed or cable pulled), so the port falls back to Improv/idle.
#ifndef VOYAGER_NET_SERIAL_H
#define VOYAGER_NET_SERIAL_H

#include <Arduino.h>

#define NET_IDLE_TIMEOUT_MS 30000

void net_init();                 // queue + writer task; call once after the LoRa init
bool net_active();
bool net_enter();                // false if the file-transfer bridge owns the port
void net_leave(const char* why);
void net_feed(uint8_t b);        // USB byte while net_active()
void net_poll();                 // idle timeout; call periodically from the serial task

// Called from the LoRa RX task.
bool net_deliver(const uint8_t* ip, size_t len);   // false: not active or queue full
void net_heard(uint16_t node, int rssi);

// For the web UI: {"active","node","ip","tx","rx","drop","peers":[{"ip","rssi","age_s"}]}
String net_status_json();

#endif
