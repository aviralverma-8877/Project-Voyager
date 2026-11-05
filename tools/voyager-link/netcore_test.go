package main

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"
)

func ipv4Packet(dst [4]byte, proto byte, payload []byte) []byte {
	p := make([]byte, 20+len(payload))
	p[0] = 0x45
	binary.BigEndian.PutUint16(p[2:4], uint16(len(p)))
	p[8] = 64
	p[9] = proto
	copy(p[12:16], []byte{10, 73, 0xE8, 0xEC})
	copy(p[16:20], dst[:])
	copy(p[20:], payload)
	binary.BigEndian.PutUint16(p[10:12], checksum(p[:20], 0))
	return p
}

func checksum(b []byte, initial uint32) uint16 {
	sum := initial
	for i := 0; i+1 < len(b); i += 2 {
		sum += uint32(binary.BigEndian.Uint16(b[i : i+2]))
	}
	if len(b)%2 == 1 {
		sum += uint32(b[len(b)-1]) << 8
	}
	for sum>>16 != 0 {
		sum = (sum & 0xFFFF) + sum>>16
	}
	return ^uint16(sum)
}

func tcpChecksum(p []byte) uint16 {
	ihl := int(p[0]&0x0F) * 4
	tcp := append([]byte(nil), p[ihl:]...)
	tcp[16], tcp[17] = 0, 0
	var pseudo uint32
	for i := 12; i < 20; i += 2 {
		pseudo += uint32(binary.BigEndian.Uint16(p[i : i+2]))
	}
	pseudo += 6 + uint32(len(tcp))
	return checksum(tcp, pseudo)
}

// A SYN with options MSS 1460, NOP, NOP, SACK-permitted, window scale.
func tcpSyn(dst [4]byte, mss uint16, flags byte) []byte {
	tcp := make([]byte, 32)
	binary.BigEndian.PutUint16(tcp[0:2], 50000)
	binary.BigEndian.PutUint16(tcp[2:4], 22)
	tcp[12] = 8 << 4 // 32-byte header
	tcp[13] = flags
	binary.BigEndian.PutUint16(tcp[14:16], 64240)
	copy(tcp[20:], []byte{2, 4, byte(mss >> 8), byte(mss), 1, 1, 4, 2, 3, 3, 8, 0})
	p := ipv4Packet(dst, 6, tcp)
	binary.BigEndian.PutUint16(p[36:38], tcpChecksum(p))
	return p
}

var peer = [4]byte{10, 73, 0xF1, 0x5C}
var self = [4]byte{10, 73, 0xE8, 0xEC}

func TestSlipRoundTripAndFraming(t *testing.T) {
	var all []byte
	for i := 0; i < 256; i++ {
		all = append(all, byte(i))
	}
	all = append(all, slipEnd, slipEsc, slipEnd)
	enc := slipEncode(all)
	if enc[0] != slipEnd || enc[len(enc)-1] != slipEnd || bytes.Count(enc, []byte{slipEnd}) != 2 {
		t.Fatal("frame must be END-delimited with no END inside")
	}
	d := newSlipDecoder(600)
	// split the stream at every position to exercise partial feeds
	for cut := 0; cut <= len(enc); cut += 37 {
		frames := append(d.feed(enc[:cut]), d.feed(enc[cut:])...)
		if len(frames) != 1 || !bytes.Equal(frames[0], all) {
			t.Fatalf("cut %d: got %d frames", cut, len(frames))
		}
	}
	// same encoding as the firmware (test_net: test_slip_wire_format)
	want := []byte{slipEnd, 0x01, slipEsc, slipEscEnd, 0x02, slipEsc, slipEscEsc, slipEnd}
	if !bytes.Equal(slipEncode([]byte{0x01, slipEnd, 0x02, slipEsc}), want) {
		t.Fatal("wire format differs from the firmware")
	}
}

func TestSlipDecoderDropsBadFrames(t *testing.T) {
	d := newSlipDecoder(4)
	in := []byte{slipEnd, slipEnd, 1, slipEsc, 9, slipEnd, 2, slipEnd, 1, 2, 3, 4, 5, slipEnd, 3, slipEnd}
	frames := d.feed(in)
	if len(frames) != 2 || frames[0][0] != 2 || frames[1][0] != 3 {
		t.Fatalf("frames %v", frames)
	}
}

func TestOutboundFilter(t *testing.T) {
	ok := ipv4Packet(peer, 17, make([]byte, 20))
	cases := map[string][]byte{
		"":                     ok,
		"outside 10.73.0.0/16": ipv4Packet([4]byte{224, 0, 0, 251}, 17, nil), // mDNS
		"broadcast":            ipv4Packet([4]byte{10, 73, 255, 255}, 17, nil),
		"to self":              ipv4Packet(self, 1, nil),
		"ipv6":                 append([]byte{0x60}, make([]byte, 39)...),
		"not ipv4":             ok[:len(ok)-1],
	}
	for want, p := range cases {
		if got := outboundDropReason(p, self); got != want {
			t.Errorf("want %q, got %q", want, got)
		}
	}
	if r := outboundDropReason(ipv4Packet(peer, 17, make([]byte, netMaxIP)), self); !strings.HasPrefix(r, "too big") {
		t.Errorf("oversized packet: %q", r)
	}
	if r := outboundDropReason(ipv4Packet(peer, 17, make([]byte, netMaxIP-20)), self); r != "" {
		t.Errorf("largest packet that fits must pass, got %q", r)
	}
}

func TestClampMSS(t *testing.T) {
	p := tcpSyn(peer, 1460, 0x02)
	if !clampMSS(p, netClampMSS) {
		t.Fatal("SYN with MSS 1460 not clamped")
	}
	if got := binary.BigEndian.Uint16(p[42:44]); got != netClampMSS {
		t.Fatalf("mss %d", got)
	}
	if got, want := binary.BigEndian.Uint16(p[36:38]), tcpChecksum(p); got != want {
		t.Fatalf("checksum %04x, recomputed %04x", got, want)
	}

	// SYN-ACK too; small MSS, non-SYN and non-TCP are left alone.
	sa := tcpSyn(peer, 1400, 0x12)
	if !clampMSS(sa, netClampMSS) || tcpChecksum(sa) != binary.BigEndian.Uint16(sa[36:38]) {
		t.Fatal("SYN-ACK not clamped correctly")
	}
	small := tcpSyn(peer, 150, 0x02)
	if clampMSS(small, netClampMSS) {
		t.Fatal("MSS below the limit must not change")
	}
	ack := tcpSyn(peer, 1460, 0x10)
	if clampMSS(ack, netClampMSS) {
		t.Fatal("non-SYN must not change")
	}
	udp := ipv4Packet(peer, 17, make([]byte, 30))
	if clampMSS(udp, netClampMSS) {
		t.Fatal("UDP must not change")
	}
	// The largest clamped segment fits one LoRa frame: 20 IP + 32 TCP (12 opts) + 190.
	if 20+32+netClampMSS > netMaxIP {
		t.Fatal("netClampMSS too large for one LoRa frame")
	}
}

func TestImprovNetStartPacket(t *testing.T) {
	p := improvNetStart()
	if !bytes.HasPrefix(p, []byte("IMPROV\x01\x03\x02\xF0\x00")) || p[len(p)-1] != '\n' {
		t.Fatalf("% x", p)
	}
	var sum byte
	for _, b := range p[:len(p)-2] {
		sum += b
	}
	if p[len(p)-2] != sum {
		t.Fatal("checksum")
	}
}

func netStartResult(strs ...string) []byte {
	var body []byte
	for _, s := range strs {
		body = append(body, byte(len(s)))
		body = append(body, s...)
	}
	return improvPacket(improvTypeRPCResult, append([]byte{improvCmdNetStart, byte(len(body))}, body...))
}

func TestImprovScannerAndNetStartResult(t *testing.T) {
	var s improvScanner
	stream := append([]byte("boot log... IMPRO"), netStartResult("10.73.241.92", "F15C", "1")...)
	// feed in two chunks, the header split across them
	if _, _, ok := s.feed(stream[:15]); ok {
		t.Fatal("no packet yet")
	}
	typ, data, ok := s.feed(stream[15:])
	if !ok || typ != improvTypeRPCResult {
		t.Fatalf("typ %d ok %v", typ, ok)
	}
	info, err := parseNetStart(data)
	if err != nil {
		t.Fatal(err)
	}
	if ipString(info.IP) != "10.73.241.92" || info.Node != "F15C" || info.AirFullMs != 0 {
		t.Fatalf("%+v", info)
	}
	_, data, _ = (&improvScanner{}).feed(netStartResult("10.73.241.92", "F15C", "2", "247", "31"))
	if info, err = parseNetStart(data); err != nil || info.AirFullMs != 247 || info.AirSmallMs != 31 {
		t.Fatalf("airtimes: %+v %v", info, err)
	}

	_, data, _ = (&improvScanner{}).feed(netStartResult("10.73.0.5", "0005", "1"))
	if _, err := parseNetStart(data); err == nil {
		t.Fatal("10.73.0.x must be rejected")
	}
	_, data, _ = (&improvScanner{}).feed(netStartResult("10.73.1.1", "0101", "3"))
	if _, err := parseNetStart(data); err == nil || !strings.Contains(err.Error(), "protocol") {
		t.Fatalf("protocol mismatch not reported: %v", err)
	}
	bad := netStartResult("10.73.1.1", "0101", "1")
	bad[len(bad)-2] ^= 1
	if _, _, ok := (&improvScanner{}).feed(bad); ok {
		t.Fatal("bad checksum accepted")
	}
}
