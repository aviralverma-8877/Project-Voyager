package main

// Local bridge between the device-served web page and a COM port.
//
// Chrome only exposes Web Serial on secure origins, and the device UI is plain
// http://<device-ip>. The page instead opens ws://127.0.0.1:<port>/serial here.
//
// Protocol on /serial (one client at a time; a new connection takes over):
//   text   page -> helper  {"op":"list"} | {"op":"open","port":"COM5","baud":115200} | {"op":"close"}
//   text   helper -> page  {"op":"hello","version":..} | {"op":"ports","ports":[..]} |
//                          {"op":"opened","port":..} | {"op":"closed","reason":..} | {"op":"error","msg":..}
//   binary both ways       raw serial bytes
// GET /info -> {"app":"voyager-link","version":..,"busy":bool} for detection.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

const appName = "voyager-link"

// USB-UART bridges used on ESP32 LoRa boards; ports with these IDs are marked "likely".
var knownBridges = map[string]string{
	"10C4:EA60": "CP210x",
	"1A86:7523": "CH340",
	"1A86:55D4": "CH9102",
	"0403:6001": "FT232",
	"303A:1001": "ESP32 USB",
}

type portInfo struct {
	Name   string `json:"name"`
	VID    string `json:"vid,omitempty"`
	PID    string `json:"pid,omitempty"`
	Desc   string `json:"desc,omitempty"`
	Likely bool   `json:"likely"`
	Serial string `json:"serial,omitempty"`
	IsUSB  bool   `json:"usb"`
}

func listPorts() ([]portInfo, error) {
	details, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return nil, err
	}
	out := make([]portInfo, 0, len(details))
	for _, d := range details {
		p := portInfo{Name: d.Name, IsUSB: d.IsUSB, Desc: d.Product, Serial: d.SerialNumber}
		if d.IsUSB {
			p.VID, p.PID = strings.ToUpper(d.VID), strings.ToUpper(d.PID)
			if chip, ok := knownBridges[p.VID+":"+p.PID]; ok {
				p.Likely = true
				if p.Desc == "" {
					p.Desc = chip
				}
			}
		}
		out = append(out, p)
	}
	return out, nil
}

// originAllowed accepts pages served by a Voyager device: private / link-local /
// loopback IPs and mDNS (.local) or localhost names. Any other website the user has
// open must not be able to drive the COM port. WebSockets are not covered by CORS,
// so this check is the gate.
func originAllowed(origin string, extra []string) bool {
	if origin == "" {
		return false
	}
	u, err := url.Parse(origin)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") {
		return false
	}
	host := strings.ToLower(u.Hostname())
	for _, e := range extra {
		if strings.EqualFold(e, origin) || strings.EqualFold(e, host) {
			return true
		}
	}
	if host == "localhost" || strings.HasSuffix(host, ".local") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && (ip.IsPrivate() || ip.IsLoopback() || ip.IsLinkLocalUnicast())
}

type wsMsg struct {
	typ  websocket.MessageType
	data []byte
}

type session struct {
	conn   *websocket.Conn
	origin string
	ctx    context.Context
	cancel context.CancelFunc
	out    chan wsMsg

	mu       sync.Mutex
	port     serial.Port
	portName string
	portDone chan struct{}
}

type server struct {
	version     string
	extraOrigin []string
	status      func(text string, connected bool)

	mu      sync.Mutex
	current *session
}

func (s *server) busy() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.current != nil
}

func (s *server) cors(w http.ResponseWriter, r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if !originAllowed(origin, s.extraOrigin) {
		return false
	}
	h := w.Header()
	h.Set("Access-Control-Allow-Origin", origin)
	h.Set("Vary", "Origin")
	// Private Network Access / Local Network Access: a page on a LAN address talking
	// to loopback needs this on the preflight.
	h.Set("Access-Control-Allow-Private-Network", "true")
	h.Set("Access-Control-Allow-Methods", "GET, OPTIONS")
	return true
}

func (s *server) handleInfo(w http.ResponseWriter, r *http.Request) {
	allowed := s.cors(w, r)
	if r.Method == http.MethodOptions {
		if !allowed {
			w.WriteHeader(http.StatusForbidden)
			return
		}
		w.WriteHeader(http.StatusNoContent)
		return
	}
	// No Origin (curl, the single-instance probe) is fine for /info: it reveals nothing.
	if !allowed && r.Header.Get("Origin") != "" {
		http.Error(w, "origin not allowed", http.StatusForbidden)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{"app": appName, "version": s.version, "busy": s.busy()})
}

func (s *server) handleSerial(w http.ResponseWriter, r *http.Request) {
	origin := r.Header.Get("Origin")
	if !originAllowed(origin, s.extraOrigin) {
		log.Printf("rejected /serial from origin %q", origin)
		http.Error(w, "origin not allowed", http.StatusForbidden)
		return
	}
	// Origin was verified above against our own rules (coder/websocket's default
	// same-host check would reject every device page).
	c, err := websocket.Accept(w, r, &websocket.AcceptOptions{InsecureSkipVerify: true})
	if err != nil {
		log.Printf("ws accept: %v", err)
		return
	}
	c.SetReadLimit(1 << 20)

	ctx, cancel := context.WithCancel(context.Background())
	sess := &session{conn: c, origin: origin, ctx: ctx, cancel: cancel, out: make(chan wsMsg, 256)}

	s.mu.Lock()
	old := s.current
	s.current = sess
	s.mu.Unlock()
	if old != nil {
		log.Printf("new client %s takes over from %s", origin, old.origin)
		old.closePort("taken over by another page")
		old.cancel()
		old.conn.Close(websocket.StatusPolicyViolation, "another page connected")
	}
	log.Printf("client connected: %s", origin)
	s.status("Page connected ("+hostOf(origin)+")", true)

	go sess.writer()
	sess.sendJSON(map[string]any{"op": "hello", "app": appName, "version": s.version})
	s.readLoop(sess)

	sess.closePort("page disconnected")
	cancel()
	c.CloseNow()
	s.mu.Lock()
	if s.current == sess {
		s.current = nil
		s.mu.Unlock()
		s.status("Idle - waiting for the Voyager page", false)
	} else {
		s.mu.Unlock()
	}
	log.Printf("client disconnected: %s", origin)
}

func hostOf(origin string) string {
	if u, err := url.Parse(origin); err == nil && u.Host != "" {
		return u.Host
	}
	return origin
}

func (sess *session) writer() {
	for {
		select {
		case <-sess.ctx.Done():
			return
		case m := <-sess.out:
			wctx, cancel := context.WithTimeout(sess.ctx, 10*time.Second)
			err := sess.conn.Write(wctx, m.typ, m.data)
			cancel()
			if err != nil {
				sess.cancel()
				return
			}
		}
	}
}

func (sess *session) send(m wsMsg) {
	select {
	case sess.out <- m:
	case <-sess.ctx.Done():
	}
}

func (sess *session) sendJSON(v any) {
	b, _ := json.Marshal(v)
	sess.send(wsMsg{websocket.MessageText, b})
}

func (s *server) readLoop(sess *session) {
	for {
		typ, data, err := sess.conn.Read(sess.ctx)
		if err != nil {
			return
		}
		if typ == websocket.MessageBinary {
			sess.mu.Lock()
			p := sess.port
			sess.mu.Unlock()
			if p == nil {
				sess.sendJSON(map[string]any{"op": "error", "msg": "no serial port open"})
				continue
			}
			if _, err := p.Write(data); err != nil {
				sess.closePort("write failed: " + err.Error())
			}
			continue
		}
		var req struct {
			Op   string `json:"op"`
			Port string `json:"port"`
			Baud int    `json:"baud"`
		}
		if json.Unmarshal(data, &req) != nil {
			sess.sendJSON(map[string]any{"op": "error", "msg": "bad request"})
			continue
		}
		switch req.Op {
		case "list":
			ports, err := listPorts()
			if err != nil {
				sess.sendJSON(map[string]any{"op": "error", "msg": "cannot list ports: " + err.Error()})
				continue
			}
			sess.sendJSON(map[string]any{"op": "ports", "ports": ports})
		case "open":
			if err := sess.openPort(req.Port, req.Baud); err != nil {
				sess.sendJSON(map[string]any{"op": "error", "msg": err.Error()})
				continue
			}
			s.status(fmt.Sprintf("%s open for %s", req.Port, hostOf(sess.origin)), true)
			sess.sendJSON(map[string]any{"op": "opened", "port": req.Port})
		case "close":
			sess.closePort("closed by page")
			s.status("Page connected ("+hostOf(sess.origin)+")", true)
		default:
			sess.sendJSON(map[string]any{"op": "error", "msg": "unknown op " + req.Op})
		}
	}
}

func (sess *session) openPort(name string, baud int) error {
	if name == "" {
		return errors.New("no port given")
	}
	if baud <= 0 {
		baud = 115200
	}
	sess.closePort("reopening")
	// DTR and RTS low from the first instant: on ESP32 dev boards these drive EN/IO0
	// through the auto-reset circuit, and toggling them reboots (or bricks into the
	// bootloader) the device mid-transfer.
	mode := &serial.Mode{
		BaudRate:          baud,
		DataBits:          8,
		Parity:            serial.NoParity,
		StopBits:          serial.OneStopBit,
		InitialStatusBits: &serial.ModemOutputBits{DTR: false, RTS: false},
	}
	p, err := serial.Open(name, mode)
	if err != nil {
		return fmt.Errorf("cannot open %s: %v (is a serial monitor using it?)", name, err)
	}
	p.SetDTR(false)
	p.SetRTS(false)
	if err := p.SetReadTimeout(50 * time.Millisecond); err != nil {
		p.Close()
		return fmt.Errorf("cannot configure %s: %v", name, err)
	}
	p.ResetInputBuffer()

	done := make(chan struct{})
	sess.mu.Lock()
	sess.port, sess.portName, sess.portDone = p, name, done
	sess.mu.Unlock()
	log.Printf("opened %s @ %d", name, baud)

	go func() {
		defer close(done)
		buf := make([]byte, 4096)
		for {
			n, err := p.Read(buf)
			if n > 0 {
				chunk := make([]byte, n)
				copy(chunk, buf[:n])
				sess.send(wsMsg{websocket.MessageBinary, chunk})
			}
			if err != nil {
				sess.mu.Lock()
				still := sess.port == p
				sess.mu.Unlock()
				if still { // not closed by us: unplugged or driver error
					go sess.closePort("port lost: " + err.Error())
				}
				return
			}
			if sess.ctx.Err() != nil {
				return
			}
		}
	}()
	return nil
}

func (sess *session) closePort(reason string) {
	sess.mu.Lock()
	p, name, done := sess.port, sess.portName, sess.portDone
	sess.port, sess.portName, sess.portDone = nil, "", nil
	sess.mu.Unlock()
	if p == nil {
		return
	}
	p.Close()
	if done != nil {
		select {
		case <-done:
		case <-time.After(2 * time.Second):
		}
	}
	log.Printf("closed %s: %s", name, reason)
	if sess.ctx.Err() == nil {
		sess.sendJSON(map[string]any{"op": "closed", "port": name, "reason": reason})
	}
}

// Ports the page probes, in order. Keep in sync with HELPER_PORTS in data_main/script.js.
var helperPorts = []int{47321, 47322, 47323, 47324, 47325}

// listen binds the first free helper port on loopback. If an existing voyager-link
// answers on one, errAlreadyRunning is returned so a second copy exits quietly.
var errAlreadyRunning = errors.New("voyager-link is already running")

func listen() (net.Listener, int, error) {
	for _, port := range helperPorts {
		addr := fmt.Sprintf("127.0.0.1:%d", port)
		ln, err := net.Listen("tcp", addr)
		if err == nil {
			return ln, port, nil
		}
		if probeIsHelper(addr) {
			return nil, port, errAlreadyRunning
		}
	}
	return nil, 0, fmt.Errorf("all helper ports %v are in use", helperPorts)
}

func probeIsHelper(addr string) bool {
	cl := http.Client{Timeout: 500 * time.Millisecond}
	resp, err := cl.Get("http://" + addr + "/info")
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	var info struct {
		App string `json:"app"`
	}
	return json.NewDecoder(resp.Body).Decode(&info) == nil && info.App == appName
}

func (s *server) serve(ln net.Listener) error {
	mux := http.NewServeMux()
	mux.HandleFunc("/info", s.handleInfo)
	mux.HandleFunc("/serial", s.handleSerial)
	srv := &http.Server{Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	return srv.Serve(ln)
}
