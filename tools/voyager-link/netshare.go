package main

// Internet sharing over the LoRa network, platform-independent part:
//   - a gateway PC announces itself with a beacon (UDP broadcast to 10.73.255.255:47330);
//   - a client PC that chose "use shared internet" sends every internet-bound packet
//     to the gateway's node with the board's "send to node" control frame (the board
//     normally routes by destination IP, and an internet address names no node);
//   - the gateway feeds those packets to its user-space NAT (netgw.go).

import (
	"bytes"
	"fmt"
	"encoding/binary"
	"net/netip"
)

const (
	netCtlSendTo = 0x03 // 00 03 [node u16] + IPv4 (firmware protocol "2")

	gwBeaconPort  = 47330
	gwBeaconMagic = "VOYAGER-GW/1"
)

// sendToFrame wraps an IPv4 packet so the board sends it to node (the gateway).
func sendToFrame(node uint16, pkt []byte) []byte {
	f := make([]byte, 4+len(pkt))
	f[0], f[1] = 0x00, netCtlSendTo
	binary.BigEndian.PutUint16(f[2:4], node)
	copy(f[4:], pkt)
	return f
}

func nodeOf(ip [4]byte) uint16 { return uint16(ip[2])<<8 | uint16(ip[3]) }

// gwBeacon builds the gateway announcement: IPv4/UDP from the gateway to the LoRa
// broadcast address (UDP checksum 0 = none, allowed on IPv4).
func gwBeacon(self [4]byte) []byte {
	payload := []byte(gwBeaconMagic)
	p := make([]byte, 28+len(payload))
	p[0] = 0x45
	binary.BigEndian.PutUint16(p[2:4], uint16(len(p)))
	p[8] = 1 // TTL: never leaves the LoRa network
	p[9] = 17
	copy(p[12:16], self[:])
	copy(p[16:20], []byte{netPrefixA, netPrefixB, 255, 255})
	binary.BigEndian.PutUint16(p[10:12], ipChecksum(p[:20]))
	binary.BigEndian.PutUint16(p[20:22], gwBeaconPort)
	binary.BigEndian.PutUint16(p[22:24], gwBeaconPort)
	binary.BigEndian.PutUint16(p[24:26], uint16(8+len(payload)))
	copy(p[28:], payload)
	return p
}

// parseGwBeacon returns the gateway's address if p is a beacon.
func parseGwBeacon(p []byte) ([4]byte, bool) {
	var src [4]byte
	if !ipv4OK(p) || p[9] != 17 || int(p[0]&0x0F)*4 != 20 || len(p) < 28 {
		return src, false
	}
	if binary.BigEndian.Uint16(p[22:24]) != gwBeaconPort || !bytes.Equal(p[28:], []byte(gwBeaconMagic)) {
		return src, false
	}
	copy(src[:], p[12:16])
	if src[0] != netPrefixA || src[1] != netPrefixB || src[2] == 0 || src[2] == 255 || src[3] == 0 || src[3] == 255 {
		return src, false
	}
	return src, true
}

func ipChecksum(h []byte) uint16 {
	var sum uint32
	for i := 0; i+1 < len(h); i += 2 {
		sum += uint32(binary.BigEndian.Uint16(h[i : i+2]))
	}
	for sum>>16 != 0 {
		sum = (sum & 0xFFFF) + sum>>16
	}
	return ^uint16(sum)
}

// isPublicUnicast: a destination the gateway may reach for others. Private,
// loopback, link-local, CGNAT, multicast and reserved ranges are refused, so peers
// get the internet but never the gateway's own LAN or machine.
func isPublicUnicast(ip [4]byte) bool {
	a := netip.AddrFrom4(ip)
	if !a.IsGlobalUnicast() || a.IsPrivate() || a.IsLoopback() || a.IsLinkLocalUnicast() {
		return false
	}
	if ip[0] == 100 && ip[1]&0xC0 == 64 { // 100.64.0.0/10 carrier-grade NAT
		return false
	}
	if ip[0] == 0 || ip[0] >= 240 { // "this network", reserved
		return false
	}
	return true
}

// outbound decides what to do with a packet read from the adapter.
type outAction int

const (
	outDrop   outAction = iota
	outDirect           // to another LoRa node by its 10.73 address
	outViaGW            // internet: to the gateway node in a send-to frame
)

// classifyOutbound extends outboundDropReason with the shared-internet route:
// with useGW set, unicast IPv4 to a public address goes to the gateway node.
func classifyOutbound(p []byte, self [4]byte, gw *[4]byte) (outAction, string) {
	why := outboundDropReason(p, self)
	if why == "" {
		return outDirect, ""
	}
	if why != "outside 10.73.0.0/16" || gw == nil {
		return outDrop, why
	}
	var dst [4]byte
	copy(dst[:], p[16:20])
	if !isPublicUnicast(dst) {
		return outDrop, "not a public internet address"
	}
	// The 00 03 [node] header only exists on USB; the board sends the bare packet,
	// so the limit is the same one LoRa frame as for direct traffic.
	if len(p) > netMaxIP {
		return outDrop, fmt.Sprintf("too big (%d > %d bytes)", len(p), netMaxIP)
	}
	return outViaGW, ""
}
