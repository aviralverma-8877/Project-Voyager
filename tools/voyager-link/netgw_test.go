package main

import (
	"bytes"
	"context"
	"io"
	"net"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"gvisor.dev/gvisor/pkg/buffer"
	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/adapters/gonet"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/link/channel"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/udp"
)

// lab: a client stack (a LoRa peer, 10.73.1.2) wired to a gateway through an
// in-memory "radio" that, like the real one, carries only packets <= netMaxIP.
type lab struct {
	client   *stack.Stack
	gw       *gateway
	oversize atomic.Int32
	cancel   context.CancelFunc
}

func newLab(t *testing.T, dial func(network, addr string) (net.Conn, error)) *lab {
	t.Helper()
	cs := stack.New(stack.Options{
		NetworkProtocols:   []stack.NetworkProtocolFactory{ipv4.NewProtocol},
		TransportProtocols: []stack.TransportProtocolFactory{tcp.NewProtocol, udp.NewProtocol},
	})
	// Client link MTU 576 like the Windows adapter; its TCP is clamped like the
	// real Voyager Link does on the way out.
	cep := channel.New(256, netAdapterMTU, "")
	if err := cs.CreateNIC(1, cep); err != nil {
		t.Fatal(err)
	}
	cs.AddProtocolAddress(1, tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: tcpip.AddrFrom4([4]byte{10, 73, 1, 2}).WithPrefix(),
	}, stack.AddressProperties{})
	cs.SetRouteTable([]tcpip.Route{{Destination: header.IPv4EmptySubnet, NIC: 1}})

	l := &lab{client: cs}
	toClient := func(p []byte) {
		if len(p) > netMaxIP {
			l.oversize.Add(1)
			return
		}
		pb := stack.NewPacketBuffer(stack.PacketBufferOptions{Payload: buffer.MakeWithData(p)})
		cep.InjectInbound(ipv4.ProtocolNumber, pb)
		pb.DecRef()
	}
	allow := func(dst [4]byte) bool { return isPublicUnicast(dst) }
	gw, err := newGatewayWith(toClient, allow, dial)
	if err != nil {
		t.Fatal(err)
	}
	l.gw = gw
	ctx, cancel := context.WithCancel(context.Background())
	l.cancel = cancel
	go func() { // client -> "radio" -> gateway, through the same checks as netbridge
		for {
			pkt := cep.ReadContext(ctx)
			if pkt == nil {
				return
			}
			v := pkt.ToView()
			b := append([]byte(nil), v.AsSlice()...)
			v.Release()
			pkt.DecRef()
			clampMSS(b, netClampMSS)
			act, _ := classifyOutbound(b, [4]byte{10, 73, 1, 2}, &[4]byte{10, 73, 9, 9})
			if act != outViaGW {
				continue
			}
			if len(b) > netMaxIP {
				l.oversize.Add(1)
				continue
			}
			gw.inject(b)
		}
	}()
	t.Cleanup(func() { cancel(); gw.close(); cs.Close() })
	return l
}

func echoServer(t *testing.T, network string) string {
	t.Helper()
	if network == "udp" {
		pc, err := net.ListenPacket("udp", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { pc.Close() })
		go func() {
			buf := make([]byte, 2048)
			for {
				n, a, err := pc.ReadFrom(buf)
				if err != nil {
					return
				}
				pc.WriteTo(append([]byte("echo:"), buf[:n]...), a)
			}
		}()
		return pc.LocalAddr().String()
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func() { io.Copy(c, c); c.Close() }()
		}
	}()
	return ln.Addr().String()
}

// redirect: the gateway "dials the internet" but lands on the local echo server.
func redirect(t *testing.T, tcpAddr, udpAddr string, seen *atomic.Value) func(string, string) (net.Conn, error) {
	return func(network, addr string) (net.Conn, error) {
		seen.Store(network + " " + addr)
		if network == "udp" {
			return net.Dial("udp", udpAddr)
		}
		return net.Dial("tcp", tcpAddr)
	}
}

func TestGatewayTCPEndToEnd(t *testing.T) {
	var seen atomic.Value
	l := newLab(t, redirect(t, echoServer(t, "tcp"), echoServer(t, "udp"), &seen))
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	c, err := gonet.DialContextTCP(ctx, l.client,
		tcpip.FullAddress{NIC: 1, Addr: tcpip.AddrFrom4([4]byte{93, 184, 216, 34}), Port: 443}, ipv4.ProtocolNumber)
	if err != nil {
		t.Fatalf("dial through gateway: %v", err)
	}
	defer c.Close()
	if got := seen.Load(); got != "tcp 93.184.216.34:443" {
		t.Fatalf("gateway dialed %v", got)
	}
	// 20 KB both ways: many segments, all of which must fit one LoRa frame.
	msg := bytes.Repeat([]byte("voyager-lora-gateway "), 1000)
	go c.Write(msg)
	got := make([]byte, len(msg))
	c.SetReadDeadline(time.Now().Add(15 * time.Second))
	if _, err := io.ReadFull(c, got); err != nil {
		t.Fatalf("read echo: %v", err)
	}
	if !bytes.Equal(got, msg) {
		t.Fatal("echo differs")
	}
	if n := l.oversize.Load(); n != 0 {
		t.Fatalf("%d packets were too big for a LoRa frame", n)
	}
	if l.gw.TCP.Load() != 1 {
		t.Fatalf("TCP flows %d", l.gw.TCP.Load())
	}
}

func TestGatewayUDPEndToEnd(t *testing.T) {
	var seen atomic.Value
	l := newLab(t, redirect(t, echoServer(t, "tcp"), echoServer(t, "udp"), &seen))
	c, err := gonet.DialUDP(l.client, nil,
		&tcpip.FullAddress{NIC: 1, Addr: tcpip.AddrFrom4([4]byte{1, 1, 1, 1}), Port: 53}, ipv4.ProtocolNumber)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	// A 400-byte reply comes back fragmented into frame-sized IP fragments.
	query := bytes.Repeat([]byte("q"), 200)
	if _, err := c.Write(query); err != nil {
		t.Fatal(err)
	}
	c.SetReadDeadline(time.Now().Add(10 * time.Second))
	buf := make([]byte, 2048)
	n, err := c.Read(buf)
	if err != nil {
		t.Fatalf("no UDP reply: %v", err)
	}
	if !strings.HasPrefix(string(buf[:n]), "echo:") || n != 5+len(query) {
		t.Fatalf("reply %d bytes", n)
	}
	if seen.Load() != "udp 1.1.1.1:53" {
		t.Fatalf("gateway dialed %v", seen.Load())
	}
}

func TestGatewayRefusesPrivateDestinations(t *testing.T) {
	var dialed atomic.Bool
	l := newLab(t, func(network, addr string) (net.Conn, error) {
		dialed.Store(true)
		return nil, io.EOF
	})
	// The client side never even forwards it (classifyOutbound), so inject directly
	// a SYN to the gateway's LAN, as a malicious peer could.
	syn := tcpSyn([4]byte{192, 168, 1, 1}, 1460, 0x02)
	l.gw.inject(syn)
	time.Sleep(200 * time.Millisecond)
	if dialed.Load() {
		t.Fatal("gateway dialed a private address")
	}
	if l.gw.Refused.Load() == 0 {
		t.Fatal("refusal not counted")
	}
}

func TestIsPublicUnicast(t *testing.T) {
	cases := map[[4]byte]bool{
		{93, 184, 216, 34}: true, {1, 1, 1, 1}: true, {8, 8, 8, 8}: true,
		{10, 0, 0, 1}: false, {10, 73, 1, 1}: false, {192, 168, 1, 37}: false, {172, 16, 0, 1}: false,
		{127, 0, 0, 1}: false, {169, 254, 1, 1}: false, {100, 64, 0, 1}: false, {224, 0, 0, 251}: false,
		{255, 255, 255, 255}: false, {0, 0, 0, 0}: false, {240, 0, 0, 1}: false,
	}
	for ip, want := range cases {
		if got := isPublicUnicast(ip); got != want {
			t.Errorf("%v: got %v want %v", ip, got, want)
		}
	}
}

func TestBeaconAndSendTo(t *testing.T) {
	gw := [4]byte{10, 73, 0xE8, 0xEC}
	b := gwBeacon(gw)
	if !ipv4OK(b) || ipChecksum(b[:20]) != 0 {
		t.Fatal("beacon is not a valid IPv4 packet")
	}
	got, ok := parseGwBeacon(b)
	if !ok || got != gw {
		t.Fatalf("parse: %v %v", got, ok)
	}
	if outboundDropReason(b, [4]byte{10, 73, 1, 2}) != "broadcast" {
		t.Fatal("a beacon must never be forwarded from the adapter")
	}
	b[30] ^= 1
	if _, ok := parseGwBeacon(b); ok {
		t.Fatal("wrong magic accepted")
	}

	pkt := ipv4Packet([4]byte{8, 8, 8, 8}, 17, make([]byte, 30))
	f := sendToFrame(nodeOf(gw), pkt)
	if f[0] != 0 || f[1] != netCtlSendTo || f[2] != 0xE8 || f[3] != 0xEC || !bytes.Equal(f[4:], pkt) {
		t.Fatalf("send-to frame % x", f[:4])
	}
}

func TestClassifyOutbound(t *testing.T) {
	self := [4]byte{10, 73, 1, 2}
	gw := &[4]byte{10, 73, 9, 9}
	inet := ipv4Packet([4]byte{8, 8, 8, 8}, 17, make([]byte, 20))
	if a, _ := classifyOutbound(inet, self, nil); a != outDrop {
		t.Fatal("internet without a gateway must drop")
	}
	if a, _ := classifyOutbound(inet, self, gw); a != outViaGW {
		t.Fatal("internet with a gateway must go via it")
	}
	if a, _ := classifyOutbound(ipv4Packet(peer, 17, nil), self, gw); a != outDirect {
		t.Fatal("LoRa peers stay direct")
	}
	if a, why := classifyOutbound(ipv4Packet([4]byte{192, 168, 1, 1}, 17, nil), self, gw); a != outDrop || why == "" {
		t.Fatal("private LAN addresses never go to the gateway")
	}
	if a, _ := classifyOutbound(ipv4Packet([4]byte{224, 0, 0, 251}, 17, nil), self, gw); a != outDrop {
		t.Fatal("multicast must drop")
	}
	if a, _ := classifyOutbound(ipv4Packet([4]byte{8, 8, 8, 8}, 17, make([]byte, netMaxIP-20)), self, gw); a != outViaGW {
		t.Fatal("a packet that fills one frame exactly must go")
	}
	if a, _ := classifyOutbound(ipv4Packet([4]byte{8, 8, 8, 8}, 17, make([]byte, netMaxIP-19)), self, gw); a != outDrop {
		t.Fatal("one byte over a frame must drop")
	}
}
