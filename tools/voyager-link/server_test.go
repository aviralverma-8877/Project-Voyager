package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func TestOriginAllowed(t *testing.T) {
	cases := map[string]bool{
		"http://192.168.4.1":         true, // device AP
		"http://10.0.0.23":           true,
		"http://172.20.1.5:8080":     true,
		"http://voyager.local":       true,
		"http://localhost:5500":      true,
		"http://127.0.0.1":           true,
		"http://169.254.10.2":        true,
		"https://evil.example.com":   false,
		"http://8.8.8.8":             false,
		"http://172.32.0.1":          false, // outside 172.16/12
		"http://192.168.4.1.evil.io": false,
		"file://":                    false,
		"null":                       false,
		"":                           false,
	}
	for origin, want := range cases {
		if got := originAllowed(origin, nil); got != want {
			t.Errorf("originAllowed(%q) = %v, want %v", origin, got, want)
		}
	}
	if !originAllowed("https://voyager.example.org", []string{"voyager.example.org"}) {
		t.Error("extra host not honoured")
	}
}

func newTestServer(t *testing.T) (*server, *httptest.Server) {
	s := &server{version: "test", status: func(string, bool) {}}
	mux := http.NewServeMux()
	mux.HandleFunc("/info", s.handleInfo)
	mux.HandleFunc("/serial", s.handleSerial)
	ts := httptest.NewServer(mux)
	t.Cleanup(ts.Close)
	return s, ts
}

func dial(t *testing.T, ts *httptest.Server, origin string) (*websocket.Conn, *http.Response, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	h := http.Header{}
	if origin != "" {
		h.Set("Origin", origin)
	}
	return websocket.Dial(ctx, "ws"+strings.TrimPrefix(ts.URL, "http")+"/serial", &websocket.DialOptions{HTTPHeader: h})
}

func readJSON(t *testing.T, c *websocket.Conn) map[string]any {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	typ, data, err := c.Read(ctx)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if typ != websocket.MessageText {
		t.Fatalf("expected text frame")
	}
	var m map[string]any
	if err := json.Unmarshal(data, &m); err != nil {
		t.Fatalf("bad json %q", data)
	}
	return m
}

func writeJSON(t *testing.T, c *websocket.Conn, v any) {
	b, _ := json.Marshal(v)
	if err := c.Write(context.Background(), websocket.MessageText, b); err != nil {
		t.Fatalf("write: %v", err)
	}
}

func TestSerialRejectsForeignOrigin(t *testing.T) {
	_, ts := newTestServer(t)
	_, resp, err := dial(t, ts, "https://evil.example.com")
	if err == nil {
		t.Fatal("foreign origin was accepted")
	}
	if resp == nil || resp.StatusCode != http.StatusForbidden {
		t.Fatalf("want 403, got %v", resp)
	}
	if _, _, err := dial(t, ts, ""); err == nil {
		t.Fatal("missing origin was accepted")
	}
}

func TestSerialSessionFlow(t *testing.T) {
	s, ts := newTestServer(t)
	c, _, err := dial(t, ts, "http://192.168.4.1")
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer c.CloseNow()

	if m := readJSON(t, c); m["op"] != "hello" || m["app"] != appName {
		t.Fatalf("hello: %v", m)
	}
	if !s.busy() {
		t.Fatal("server should report busy while a page is connected")
	}

	writeJSON(t, c, map[string]any{"op": "list"})
	if m := readJSON(t, c); m["op"] != "ports" {
		t.Fatalf("list: %v", m)
	}

	// Binary data before a port is open is refused, not dropped silently.
	c.Write(context.Background(), websocket.MessageBinary, []byte{0x43})
	if m := readJSON(t, c); m["op"] != "error" {
		t.Fatalf("binary without port: %v", m)
	}

	writeJSON(t, c, map[string]any{"op": "open", "port": "COM_DOES_NOT_EXIST_99"})
	if m := readJSON(t, c); m["op"] != "error" {
		t.Fatalf("open bogus port: %v", m)
	}

	writeJSON(t, c, map[string]any{"op": "nope"})
	if m := readJSON(t, c); m["op"] != "error" {
		t.Fatalf("unknown op: %v", m)
	}
}

// Two tabs (one per board) on the same PC must both stay connected.
func TestTwoPagesStayConnected(t *testing.T) {
	_, ts := newTestServer(t)
	a, _, err := dial(t, ts, "http://192.168.1.37")
	if err != nil {
		t.Fatal(err)
	}
	defer a.CloseNow()
	readJSON(t, a)
	b, _, err := dial(t, ts, "http://192.168.1.38")
	if err != nil {
		t.Fatal(err)
	}
	defer b.CloseNow()
	readJSON(t, b)

	// The first page still answers after the second connected.
	writeJSON(t, a, map[string]any{"op": "list"})
	if m := readJSON(t, a); m["op"] != "ports" {
		t.Fatalf("first page lost its session: %v", m)
	}
	writeJSON(t, b, map[string]any{"op": "list"})
	if m := readJSON(t, b); m["op"] != "ports" {
		t.Fatalf("second page: %v", m)
	}
}

func TestPortOwnership(t *testing.T) {
	s := &server{status: func(string, bool) {}}
	a := &session{srv: s, origin: "http://192.168.1.37"}
	b := &session{srv: s, origin: "http://192.168.1.38"}
	if err := s.claim("COM18", a); err != nil {
		t.Fatal(err)
	}
	if err := s.claim("com18", b); err == nil || !strings.Contains(err.Error(), "192.168.1.37") {
		t.Fatalf("second page must be refused and told who holds the port, got %v", err)
	}
	if err := s.claim("COM13", b); err != nil {
		t.Fatalf("a different port must be free: %v", err)
	}
	if err := s.claim("COM18", a); err != nil {
		t.Fatalf("owner may re-claim: %v", err)
	}
	s.release("COM18", b) // not the owner: no effect
	if err := s.claim("COM18", b); err == nil {
		t.Fatal("release by a non-owner freed the port")
	}
	s.release("COM18", a)
	if err := s.claim("COM18", b); err != nil {
		t.Fatalf("port should be free after release: %v", err)
	}
}

func TestTooManyPages(t *testing.T) {
	_, ts := newTestServer(t)
	var conns []*websocket.Conn
	defer func() {
		for _, c := range conns {
			c.CloseNow()
		}
	}()
	for i := 0; i < maxSessions; i++ {
		c, _, err := dial(t, ts, "http://192.168.1.37")
		if err != nil {
			t.Fatal(err)
		}
		readJSON(t, c)
		conns = append(conns, c)
	}
	c, _, err := dial(t, ts, "http://192.168.1.37")
	if err != nil {
		t.Fatal(err)
	}
	defer c.CloseNow()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if _, _, err := c.Read(ctx); websocket.CloseStatus(err) != websocket.StatusTryAgainLater {
		t.Fatalf("page %d should be refused with 1013, got %v", maxSessions+1, err)
	}
}

func TestInfoCORS(t *testing.T) {
	_, ts := newTestServer(t)
	req, _ := http.NewRequest(http.MethodOptions, ts.URL+"/info", nil)
	req.Header.Set("Origin", "http://192.168.4.1")
	req.Header.Set("Access-Control-Request-Private-Network", "true")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNoContent ||
		resp.Header.Get("Access-Control-Allow-Private-Network") != "true" ||
		resp.Header.Get("Access-Control-Allow-Origin") != "http://192.168.4.1" {
		t.Fatalf("preflight: %d %v", resp.StatusCode, resp.Header)
	}

	req, _ = http.NewRequest(http.MethodGet, ts.URL+"/info", nil)
	req.Header.Set("Origin", "https://evil.example.com")
	resp, _ = http.DefaultClient.Do(req)
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("foreign /info: %d", resp.StatusCode)
	}

	resp, _ = http.Get(ts.URL + "/info")
	var info map[string]any
	json.NewDecoder(resp.Body).Decode(&info)
	if info["app"] != appName {
		t.Fatalf("info: %v", info)
	}
}

func TestMakeIconIsValidICO(t *testing.T) {
	ico := makeIcon(1, 2, 3)
	if len(ico) != 6+16+40+32*32*4+32*4 {
		t.Fatalf("unexpected ico size %d", len(ico))
	}
	if ico[2] != 1 || ico[4] != 1 {
		t.Fatal("bad ICO header")
	}
}
