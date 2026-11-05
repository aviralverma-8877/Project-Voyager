package main

// Platform-independent pieces of the LoRa network bridge (see netbridge.go):
// SLIP framing, the packet filter, TCP MSS clamping and the Improv command that
// switches the board's USB serial into network mode. Kept free of OS calls so
// netcore_test.go covers them on any machine.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"time"
)

const (
	// Matches the firmware (src/lora/lora_proto.h).
	netPrefixA = 10
	netPrefixB = 73
	netMaxIP   = 247 // one LoRa frame: 251 payload - 4 byte [dst][src] header

	// Windows will not go below 576 on an IPv4 interface. TCP segments are kept
	// within one LoRa frame by clamping the MSS instead: 20 IP + 20 TCP + up to 12
	// bytes of options + 190 data = 242 <= netMaxIP.
	netAdapterMTU = 576
	netClampMSS   = 190

	netCtlKeepalive = 0x01
	netCtlLeave     = 0x02
	netCtlAliveAck  = 0x81 // board -> PC: still in network mode (answers a keepalive)
	netCtlBusy      = 0x82 // board -> PC: 00 82 [queued]: radio queue filling, slow down

	// No keepalive answer for this long: the board rebooted (or old firmware), so
	// repeat the network-start command; same board, same address.
	netAckTimeout = 20 * time.Second
)

// ---- SLIP (RFC 1055) --------------------------------------------------------------

const (
	slipEnd    = 0xC0
	slipEsc    = 0xDB
	slipEscEnd = 0xDC
	slipEscEsc = 0xDD
)

func slipEncode(p []byte) []byte {
	out := make([]byte, 0, len(p)+len(p)/8+2)
	out = append(out, slipEnd)
	for _, b := range p {
		switch b {
		case slipEnd:
			out = append(out, slipEsc, slipEscEnd)
		case slipEsc:
			out = append(out, slipEsc, slipEscEsc)
		default:
			out = append(out, b)
		}
	}
	return append(out, slipEnd)
}

// slipDecoder turns a byte stream into frames; oversized or badly escaped frames
// are dropped.
type slipDecoder struct {
	buf []byte
	max int
	esc bool
	bad bool
}

func newSlipDecoder(max int) *slipDecoder { return &slipDecoder{buf: make([]byte, 0, max), max: max} }

// feed returns the completed frames found in chunk (each a fresh slice).
func (d *slipDecoder) feed(chunk []byte) [][]byte {
	var frames [][]byte
	for _, b := range chunk {
		if b == slipEnd {
			if !d.bad && !d.esc && len(d.buf) > 0 {
				frames = append(frames, append([]byte(nil), d.buf...))
			}
			d.buf, d.esc, d.bad = d.buf[:0], false, false
			continue
		}
		if d.bad {
			continue
		}
		if d.esc {
			d.esc = false
			switch b {
			case slipEscEnd:
				b = slipEnd
			case slipEscEsc:
				b = slipEsc
			default:
				d.bad = true
				continue
			}
		} else if b == slipEsc {
			d.esc = true
			continue
		}
		if len(d.buf) >= d.max {
			d.bad = true
			continue
		}
		d.buf = append(d.buf, b)
	}
	return frames
}

// ---- packet filter ----------------------------------------------------------------

// ipv4OK: version 4, header length sane, total length == len(p).
func ipv4OK(p []byte) bool {
	if len(p) < 20 || p[0]>>4 != 4 {
		return false
	}
	ihl := int(p[0]&0x0F) * 4
	total := int(binary.BigEndian.Uint16(p[2:4]))
	return ihl >= 20 && ihl <= total && total == len(p)
}

// Why a packet from the adapter is not sent over LoRa ("" = send it).
// Only unicast IPv4 to another 10.73.x.y node goes out: Windows' broadcast and
// multicast chatter (mDNS, LLMNR, SSDP, NetBIOS, IGMP) and IPv6 would otherwise
// eat the little airtime there is.
func outboundDropReason(p []byte, self [4]byte) string {
	if len(p) == 0 || p[0]>>4 == 6 {
		return "ipv6"
	}
	if !ipv4OK(p) {
		return "not ipv4"
	}
	d := p[16:20]
	if d[0] != netPrefixA || d[1] != netPrefixB {
		return "outside 10.73.0.0/16"
	}
	if d[2] == 0 || d[2] == 255 || d[3] == 0 || d[3] == 255 {
		return "broadcast"
	}
	if d[0] == self[0] && d[1] == self[1] && d[2] == self[2] && d[3] == self[3] {
		return "to self"
	}
	if len(p) > netMaxIP {
		return fmt.Sprintf("too big (%d > %d bytes)", len(p), netMaxIP)
	}
	return ""
}

// ---- TCP MSS clamp ----------------------------------------------------------------

// clampMSS lowers the MSS option of a TCP SYN to max (fixing the TCP checksum), so
// the peer never sends segments that don't fit one LoRa frame. Returns true if it
// changed the packet. Non-TCP, non-SYN or fragmented packets are left alone.
func clampMSS(p []byte, max uint16) bool {
	if !ipv4OK(p) || p[9] != 6 {
		return false
	}
	ihl := int(p[0]&0x0F) * 4
	if binary.BigEndian.Uint16(p[6:8])&0x1FFF != 0 { // not the first fragment
		return false
	}
	tcp := p[ihl:]
	if len(tcp) < 20 || tcp[13]&0x02 == 0 { // SYN
		return false
	}
	doff := int(tcp[12]>>4) * 4
	if doff < 20 || doff > len(tcp) {
		return false
	}
	opts := tcp[20:doff]
	for i := 0; i < len(opts); {
		kind := opts[i]
		if kind == 0 { // end of options
			break
		}
		if kind == 1 { // NOP
			i++
			continue
		}
		if i+1 >= len(opts) {
			break
		}
		l := int(opts[i+1])
		if l < 2 || i+l > len(opts) {
			break
		}
		if kind == 2 && l == 4 {
			old := binary.BigEndian.Uint16(opts[i+2 : i+4])
			if old <= max {
				return false
			}
			binary.BigEndian.PutUint16(opts[i+2:i+4], max)
			// RFC 1624 incremental checksum update: HC' = ~(~HC + ~m + m')
			sum := uint32(^binary.BigEndian.Uint16(tcp[16:18])) + uint32(^old) + uint32(max)
			for sum>>16 != 0 {
				sum = (sum & 0xFFFF) + sum>>16
			}
			binary.BigEndian.PutUint16(tcp[16:18], ^uint16(sum))
			return true
		}
		i += l
	}
	return false
}

// ---- Improv: switch the board to network mode -------------------------------------

const (
	improvTypeError     = 0x02
	improvTypeRPC       = 0x03
	improvTypeRPCResult = 0x04
	improvCmdNetStart   = 0xF0 // vendor command, see src/core/improv.h
)

func improvPacket(typ byte, data []byte) []byte {
	p := append([]byte("IMPROV"), 1, typ, byte(len(data)))
	p = append(p, data...)
	var sum byte
	for _, b := range p {
		sum += b
	}
	return append(p, sum, '\n')
}

func improvNetStart() []byte { return improvPacket(improvTypeRPC, []byte{improvCmdNetStart, 0}) }

// netStartInfo is the board's answer to the network-start command.
type netStartInfo struct {
	IP    [4]byte
	Node  string
	Proto string
	// Airtime (ms) of a full / minimal NET frame under the board's radio config;
	// 0 when the firmware doesn't report them (the pacer then uses safe defaults).
	AirFullMs, AirSmallMs int
}

var errImprovRefused = errors.New("the board refused network mode (file transfer bridge on?)")

// improvScanner finds Improv packets in a serial stream (boot logs etc. around them).
type improvScanner struct{ buf []byte }

// next returns the next complete Improv packet (type, data) in the buffered stream.
func (s *improvScanner) feed(chunk []byte) (typ byte, data []byte, ok bool) {
	s.buf = append(s.buf, chunk...)
	for {
		i := indexOf(s.buf, []byte("IMPROV"))
		if i < 0 {
			if len(s.buf) > 5 {
				s.buf = s.buf[len(s.buf)-5:]
			}
			return 0, nil, false
		}
		s.buf = s.buf[i:]
		if len(s.buf) < 9 {
			return 0, nil, false
		}
		n := int(s.buf[8])
		if len(s.buf) < 9+n+1 {
			return 0, nil, false
		}
		var sum byte
		for _, b := range s.buf[:9+n] {
			sum += b
		}
		pkt := s.buf[:9+n+1]
		s.buf = s.buf[9+n+1:]
		if pkt[6] != 1 || sum != pkt[9+n] {
			continue
		}
		return pkt[7], append([]byte(nil), pkt[9:9+n]...), true
	}
}

func indexOf(b, sub []byte) int {
	for i := 0; i+len(sub) <= len(b); i++ {
		if string(b[i:i+len(sub)]) == string(sub) {
			return i
		}
	}
	return -1
}

// parseNetStart reads the RPC result strings of the network-start command.
func parseNetStart(data []byte) (netStartInfo, error) {
	var info netStartInfo
	if len(data) < 2 || data[0] != improvCmdNetStart {
		return info, errors.New("not a network-start result")
	}
	var strs []string
	for i := 2; i < len(data); {
		l := int(data[i])
		if i+1+l > len(data) {
			return info, errors.New("truncated result")
		}
		strs = append(strs, string(data[i+1:i+1+l]))
		i += 1 + l
	}
	if len(strs) < 3 {
		return info, errors.New("short result")
	}
	var a, b, c, d int
	if _, err := fmt.Sscanf(strs[0], "%d.%d.%d.%d", &a, &b, &c, &d); err != nil ||
		a != netPrefixA || b != netPrefixB || c < 1 || c > 254 || d < 1 || d > 254 {
		return info, fmt.Errorf("unexpected address %q", strs[0])
	}
	info.IP = [4]byte{byte(a), byte(b), byte(c), byte(d)}
	info.Node, info.Proto = strs[1], strs[2]
	if len(strs) >= 5 {
		fmt.Sscanf(strs[3], "%d", &info.AirFullMs)
		fmt.Sscanf(strs[4], "%d", &info.AirSmallMs)
	}
	// "1": networking. "2": adds the send-to frame used for shared internet.
	if info.Proto != "1" && info.Proto != "2" {
		return info, fmt.Errorf("board speaks network protocol %q, this Voyager Link speaks 1-2: update one of them", info.Proto)
	}
	return info, nil
}

func ipString(ip [4]byte) string { return fmt.Sprintf("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]) }
