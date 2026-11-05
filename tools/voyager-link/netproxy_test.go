package main

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"strings"
	"testing"
	"time"
)

// Test setup: the "internet" is local servers on 127.0.0.1, so the allow function
// accepts loopback here (production: isPublicUnicast), and "public.example" resolves
// to 127.0.0.1 while "lan.example" resolves to a private address.
func testProxy(t *testing.T, peerOK func(net.IP) bool) *gwProxy {
	t.Helper()
	allow := func(ip [4]byte) bool { return ip[0] == 127 || isPublicUnicast(ip) }
	resolve := func(ctx context.Context, host string) ([]netip.Addr, error) {
		switch host {
		case "public.example":
			return []netip.Addr{netip.MustParseAddr("127.0.0.1")}, nil
		case "lan.example":
			return []netip.Addr{netip.MustParseAddr("192.168.1.1")}, nil
		}
		return nil, fmt.Errorf("no such host %s", host)
	}
	p, err := newGwProxyWith("127.0.0.1:0", allow, peerOK, resolve)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(p.close)
	return p
}

func allPeers(net.IP) bool { return true }

func TestProxyPlainHTTP(t *testing.T) {
	web := http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "hello %s", r.URL.Path)
	})}
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	go web.Serve(ln)
	defer web.Close()
	_, port, _ := net.SplitHostPort(ln.Addr().String())

	p := testProxy(t, allPeers)
	pu, _ := url.Parse("http://" + p.Addr())
	cl := http.Client{Transport: &http.Transport{Proxy: http.ProxyURL(pu)}, Timeout: 10 * time.Second}
	resp, err := cl.Get("http://public.example:" + port + "/lora")
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 200 || string(body) != "hello /lora" {
		t.Fatalf("%d %q", resp.StatusCode, body)
	}

	// A name that resolves to the gateway's LAN is refused.
	resp, err = cl.Get("http://lan.example/")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("LAN destination: %d", resp.StatusCode)
	}
}

func TestProxyConnectTunnel(t *testing.T) {
	echo, _ := net.Listen("tcp", "127.0.0.1:0")
	defer echo.Close()
	go func() {
		for {
			c, err := echo.Accept()
			if err != nil {
				return
			}
			go func() { io.Copy(c, c); c.Close() }()
		}
	}()
	_, port, _ := net.SplitHostPort(echo.Addr().String())

	p := testProxy(t, allPeers)
	c, err := net.Dial("tcp", p.Addr())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	fmt.Fprintf(c, "CONNECT public.example:%s HTTP/1.1\r\nHost: public.example:%s\r\n\r\n", port, port)
	br := bufio.NewReader(c)
	status, _ := br.ReadString('\n')
	if !strings.Contains(status, " 200 ") {
		t.Fatalf("CONNECT: %q", status)
	}
	br.ReadString('\n') // blank line
	c.Write([]byte("tls-bytes"))
	got := make([]byte, 9)
	c.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, err := io.ReadFull(br, got); err != nil || string(got) != "tls-bytes" {
		t.Fatalf("tunnel: %q %v", got, err)
	}

	// CONNECT to a private address literal is refused.
	c2, _ := net.Dial("tcp", p.Addr())
	defer c2.Close()
	fmt.Fprintf(c2, "CONNECT 10.0.0.1:22 HTTP/1.1\r\nHost: 10.0.0.1:22\r\n\r\n")
	status, _ = bufio.NewReader(c2).ReadString('\n')
	if !strings.Contains(status, " 403 ") {
		t.Fatalf("private CONNECT: %q", status)
	}
}

func TestProxyOnlyServesLoRaPeers(t *testing.T) {
	p := testProxy(t, loraPeer) // the test client comes from 127.0.0.1, not 10.73.x.y
	pu, _ := url.Parse("http://" + p.Addr())
	cl := http.Client{Transport: &http.Transport{Proxy: http.ProxyURL(pu)}, Timeout: 5 * time.Second}
	resp, err := cl.Get("http://public.example/")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("non-LoRa client got %d", resp.StatusCode)
	}
	if !loraPeer(net.ParseIP("10.73.241.92")) || loraPeer(net.ParseIP("192.168.1.38")) {
		t.Fatal("loraPeer")
	}
}

func TestPacerBackoffAndRecovery(t *testing.T) {
	p := newPacer(240, 30, nil)
	base := p.cost(netMaxIP)
	p.backoff()
	if got := p.cost(netMaxIP); got < base*3/2-time.Millisecond {
		t.Fatalf("after backoff %v, base %v", got, base)
	}
	for i := 0; i < 10; i++ {
		p.backoff()
	}
	if got := p.cost(netMaxIP); got > base*4+time.Millisecond {
		t.Fatalf("backoff not capped: %v", got)
	}
	for i := 0; i < 200; i++ {
		p.recover()
	}
	if got := p.cost(netMaxIP); got != base {
		t.Fatalf("did not recover to %v: %v", base, got)
	}
}
