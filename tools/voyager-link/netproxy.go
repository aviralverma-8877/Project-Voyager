package main

// Shared internet, proxy mode (the default): the gateway runs an HTTP proxy on its
// LoRa address (gwProxyPort) and clients point the Windows proxy setting at it.
//
// Why a proxy and not a default route: with the LoRa link as default route, all of
// Windows' background traffic (Update, Store, sync, telemetry) goes through a
// ~1 KB/s link and starves whatever the user actually wants (seen on hardware: ping
// and DNS lost in a full queue). Through the proxy only apps that use the system
// proxy (browsers, most apps, curl -x) take the LoRa path, and DNS is resolved at
// the gateway instead of costing airtime.
//
// Supports CONNECT (HTTPS and any TCP) and plain http:// requests. Destinations are
// resolved at the gateway and must be public (isPublicUnicast): peers never reach
// the gateway itself or its LAN.

import (
	"context"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"net/netip"
	"strings"
	"sync"
	"time"
)

const gwProxyPort = 3128

type gwProxy struct {
	srv     *http.Server
	ln      net.Listener
	allow   func([4]byte) bool // isPublicUnicast in production
	peerOK  func(net.IP) bool  // only LoRa network clients
	dialer  net.Dialer
	resolve func(ctx context.Context, host string) ([]netip.Addr, error)
	tr      *http.Transport
	wg      sync.WaitGroup
}

func loraPeer(ip net.IP) bool {
	v4 := ip.To4()
	return v4 != nil && v4[0] == netPrefixA && v4[1] == netPrefixB
}

func newGwProxy(addr string) (*gwProxy, error) {
	return newGwProxyWith(addr, isPublicUnicast, loraPeer, nil)
}

func newGwProxyWith(addr string, allow func([4]byte) bool, peerOK func(net.IP) bool,
	resolve func(context.Context, string) ([]netip.Addr, error)) (*gwProxy, error) {
	ln, err := net.Listen("tcp4", addr)
	if err != nil {
		return nil, err
	}
	p := &gwProxy{ln: ln, allow: allow, peerOK: peerOK, resolve: resolve}
	p.dialer = net.Dialer{Timeout: gwDialTimeout}
	if p.resolve == nil {
		p.resolve = func(ctx context.Context, host string) ([]netip.Addr, error) {
			return net.DefaultResolver.LookupNetIP(ctx, "ip4", host)
		}
	}
	p.tr = &http.Transport{
		DialContext:         p.dialPublic,
		Proxy:               nil, // never chain through the gateway's own proxy
		MaxIdleConns:        16,
		IdleConnTimeout:     60 * time.Second,
		TLSHandshakeTimeout: 20 * time.Second,
	}
	p.srv = &http.Server{Handler: p, ReadHeaderTimeout: 60 * time.Second}
	p.wg.Add(1)
	go func() {
		defer p.wg.Done()
		if err := p.srv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("gateway proxy: %v", err)
		}
	}()
	return p, nil
}

func (p *gwProxy) Addr() string { return p.ln.Addr().String() }

var errNotPublic = errors.New("destination is not a public internet address")

// dialPublic resolves at the gateway and only connects to public addresses.
func (p *gwProxy) dialPublic(ctx context.Context, network, address string) (net.Conn, error) {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return nil, err
	}
	var addrs []netip.Addr
	if a, err := netip.ParseAddr(host); err == nil {
		addrs = []netip.Addr{a}
	} else if addrs, err = p.resolve(ctx, host); err != nil {
		return nil, err
	}
	var lastErr error = errNotPublic
	for _, a := range addrs {
		if !a.Is4() || !p.allow(a.As4()) {
			continue
		}
		c, err := p.dialer.DialContext(ctx, "tcp4", net.JoinHostPort(a.String(), port))
		if err == nil {
			return c, nil
		}
		lastErr = err
	}
	return nil, lastErr
}

func (p *gwProxy) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	if ip := net.ParseIP(host); ip == nil || !p.peerOK(ip) {
		http.Error(w, "only LoRa network peers may use this proxy", http.StatusForbidden)
		return
	}
	if r.Method == http.MethodConnect {
		p.connect(w, r)
		return
	}
	if r.URL.Scheme != "http" || r.URL.Host == "" {
		http.Error(w, "this is a proxy: send absolute http:// URLs or CONNECT", http.StatusBadRequest)
		return
	}
	out := r.Clone(r.Context())
	out.RequestURI = ""
	out.Header.Del("Proxy-Connection")
	out.Header.Del("Proxy-Authorization")
	resp, err := p.tr.RoundTrip(out)
	if err != nil {
		code := http.StatusBadGateway
		if errors.Is(err, errNotPublic) {
			code = http.StatusForbidden
		}
		http.Error(w, err.Error(), code)
		return
	}
	defer resp.Body.Close()
	for k, vs := range resp.Header {
		for _, v := range vs {
			w.Header().Add(k, v)
		}
	}
	w.WriteHeader(resp.StatusCode)
	io.Copy(w, resp.Body)
}

func (p *gwProxy) connect(w http.ResponseWriter, r *http.Request) {
	target := r.Host
	if !strings.Contains(target, ":") {
		target += ":443"
	}
	out, err := p.dialPublic(r.Context(), "tcp", target)
	if err != nil {
		code := http.StatusBadGateway
		if errors.Is(err, errNotPublic) {
			code = http.StatusForbidden
		}
		http.Error(w, err.Error(), code)
		return
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		out.Close()
		http.Error(w, "cannot tunnel", http.StatusInternalServerError)
		return
	}
	in, buf, err := hj.Hijack()
	if err != nil {
		out.Close()
		return
	}
	in.Write([]byte("HTTP/1.1 200 Connection established\r\n\r\n"))
	if n := buf.Reader.Buffered(); n > 0 { // bytes the client sent after the CONNECT
		b, _ := buf.Reader.Peek(n)
		out.Write(b)
	}
	p.wg.Add(1)
	go func() {
		defer p.wg.Done()
		done := make(chan struct{}, 2)
		go func() { io.Copy(out, in); done <- struct{}{} }()
		go func() { io.Copy(in, out); done <- struct{}{} }()
		<-done
		in.Close()
		out.Close()
	}()
}

func (p *gwProxy) close() {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	p.srv.Shutdown(ctx)
	p.srv.Close()
	p.tr.CloseIdleConnections()
}
