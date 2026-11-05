package main

// Internet gateway: a user-space NAT for peers on the LoRa network.
//
// Packets that LoRa peers send to the internet through this PC are injected into an
// in-process TCP/IP stack (gVisor) running in promiscuous + spoofing mode. Its TCP
// and UDP forwarders accept every flow, and each one is re-opened as an ordinary
// socket from this PC (net.Dial), with the data piped between the two. Replies come
// out of the stack as IPv4 packets addressed to the peer and go back over LoRa.
//
// No Windows ICS, routes or NAT rules are touched: the gateway PC's own networking
// is unchanged. Only public unicast destinations are allowed (isPublicUnicast), so
// peers can't reach this PC or its LAN.

import (
	"context"
	"errors"
	"io"
	"log"
	"net"
	"net/netip"
	"strconv"
	"sync"
	"sync/atomic"
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
	"gvisor.dev/gvisor/pkg/waiter"
)

const (
	// Stack MTU: TCP replies get MSS 190 and UDP replies are fragmented so every
	// packet fits one LoRa frame (netMaxIP).
	gwMTU          = 230
	gwDialTimeout  = 15 * time.Second
	gwUDPIdle      = 60 * time.Second
	gwMaxInFlight  = 32 // TCP handshakes in progress
	gwMaxFlows     = 64 // open flows; more are refused (the link can't carry them anyway)
)

type gateway struct {
	s     *stack.Stack
	ep    *channel.Endpoint
	send  func(pkt []byte)                           // towards the LoRa network
	allow func(dst [4]byte) bool                     // isPublicUnicast in production
	dial  func(network, addr string) (net.Conn, error) // net.DialTimeout in production

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
	flows  atomic.Int32
	conns  sync.Map // net.Conn -> struct{}, closed on shutdown

	TCP, UDP, Refused atomic.Uint64
}

func newGateway(send func([]byte)) (*gateway, error) {
	return newGatewayWith(send, isPublicUnicast, func(network, addr string) (net.Conn, error) {
		return net.DialTimeout(network, addr, gwDialTimeout)
	})
}

func newGatewayWith(send func([]byte), allow func([4]byte) bool,
	dial func(network, addr string) (net.Conn, error)) (*gateway, error) {
	s := stack.New(stack.Options{
		NetworkProtocols:   []stack.NetworkProtocolFactory{ipv4.NewProtocol},
		TransportProtocols: []stack.TransportProtocolFactory{tcp.NewProtocol, udp.NewProtocol},
	})
	ep := channel.New(256, gwMTU, "")
	if err := s.CreateNIC(1, ep); err != nil {
		return nil, errors.New("gateway: CreateNIC: " + err.String())
	}
	// Accept packets for any destination and answer from any source address.
	s.SetPromiscuousMode(1, true)
	s.SetSpoofing(1, true)
	s.SetRouteTable([]tcpip.Route{{Destination: header.IPv4EmptySubnet, NIC: 1}})

	ctx, cancel := context.WithCancel(context.Background())
	g := &gateway{s: s, ep: ep, send: send, allow: allow, dial: dial, ctx: ctx, cancel: cancel}
	tf := tcp.NewForwarder(s, 0, gwMaxInFlight, g.handleTCP)
	s.SetTransportProtocolHandler(tcp.ProtocolNumber, tf.HandlePacket)
	uf := udp.NewForwarder(s, g.handleUDP)
	s.SetTransportProtocolHandler(udp.ProtocolNumber, uf.HandlePacket)

	g.wg.Add(1)
	go g.outLoop()
	return g, nil
}

// inject hands the gateway one IPv4 packet a peer sent towards the internet.
func (g *gateway) inject(pkt []byte) {
	if !ipv4OK(pkt) {
		return
	}
	var dst [4]byte
	copy(dst[:], pkt[16:20])
	if !g.allow(dst) {
		g.Refused.Add(1)
		return
	}
	pb := stack.NewPacketBuffer(stack.PacketBufferOptions{Payload: buffer.MakeWithData(append([]byte(nil), pkt...))})
	g.ep.InjectInbound(ipv4.ProtocolNumber, pb)
	pb.DecRef()
}

// outLoop: packets the stack sends (replies for peers) go back over LoRa.
func (g *gateway) outLoop() {
	defer g.wg.Done()
	for {
		pkt := g.ep.ReadContext(g.ctx)
		if pkt == nil {
			return
		}
		v := pkt.ToView()
		b := append([]byte(nil), v.AsSlice()...)
		v.Release()
		pkt.DecRef()
		g.send(b)
	}
}

func target(addr tcpip.Address, port uint16) string {
	return net.JoinHostPort(netip.AddrFrom4(addr.As4()).String(), strconv.Itoa(int(port)))
}

func (g *gateway) track(c net.Conn) func() {
	g.conns.Store(c, struct{}{})
	return func() { g.conns.Delete(c); c.Close() }
}

func (g *gateway) handleTCP(r *tcp.ForwarderRequest) {
	id := r.ID()
	if !g.allow(id.LocalAddress.As4()) || g.flows.Load() >= gwMaxFlows {
		g.Refused.Add(1)
		r.Complete(true) // RST
		return
	}
	out, err := g.dial("tcp", target(id.LocalAddress, id.LocalPort))
	if err != nil {
		r.Complete(true)
		return
	}
	var wq waiter.Queue
	ep, terr := r.CreateEndpoint(&wq)
	if terr != nil {
		out.Close()
		r.Complete(true)
		return
	}
	r.Complete(false)
	in := gonet.NewTCPConn(&wq, ep)
	g.TCP.Add(1)
	g.pipe(in, out, 0)
}

func (g *gateway) handleUDP(r *udp.ForwarderRequest) {
	id := r.ID()
	if !g.allow(id.LocalAddress.As4()) || g.flows.Load() >= gwMaxFlows {
		g.Refused.Add(1)
		return
	}
	var wq waiter.Queue
	ep, terr := r.CreateEndpoint(&wq)
	if terr != nil {
		return
	}
	in := gonet.NewUDPConn(&wq, ep)
	out, err := g.dial("udp", target(id.LocalAddress, id.LocalPort))
	if err != nil {
		in.Close()
		return
	}
	g.UDP.Add(1)
	go g.pipe(in, out, gwUDPIdle)
}

// pipe copies both ways until either side ends (or, for UDP, goes idle).
func (g *gateway) pipe(a, b net.Conn, idle time.Duration) {
	g.flows.Add(1)
	g.wg.Add(1)
	closeA, closeB := g.track(a), g.track(b)
	go func() {
		defer g.wg.Done()
		defer g.flows.Add(-1)
		var once sync.Once
		done := make(chan struct{})
		stop := func() { once.Do(func() { closeA(); closeB(); close(done) }) }
		cp := func(dst, src net.Conn) {
			defer stop()
			if idle == 0 {
				io.Copy(dst, src)
				return
			}
			buf := make([]byte, 2048)
			for {
				src.SetReadDeadline(time.Now().Add(idle))
				n, err := src.Read(buf)
				if n > 0 {
					if _, werr := dst.Write(buf[:n]); werr != nil {
						return
					}
				}
				if err != nil {
					return
				}
			}
		}
		go cp(a, b)
		go cp(b, a)
		select {
		case <-done:
		case <-g.ctx.Done():
			stop()
		}
	}()
}

func (g *gateway) close() {
	g.cancel()
	g.conns.Range(func(k, _ any) bool { k.(net.Conn).Close(); return true })
	g.wg.Wait()
	g.s.Close()
	g.s.Wait()
	log.Printf("gateway: closed (%d TCP, %d UDP flows, %d refused)", g.TCP.Load(), g.UDP.Load(), g.Refused.Load())
}
