//go:build windows

package main

// LoRa network bridge on Windows: a Wintun adapter "Voyager-LoRa" with
// 10.73.x.y/16, whose IPv4 packets travel as SLIP frames over the board's COM port
// (the board relays them over LoRa as T_NET frames; see src/core/net_serial.h).

import (
	"bytes"
	"errors"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"go.bug.st/serial"
	"golang.org/x/sys/windows"
	"golang.zx2c4.com/wireguard/tun"
)

const netAdapterName = "Voyager-LoRa"

type netBridge struct {
	srv      *server
	owner    *session
	portName string
	port     serial.Port
	dev      tun.Device
	info     netStartInfo
	c        netCounters

	wmu      sync.Mutex // serial writes
	stopOnce sync.Once

	// internet sharing
	shareMu sync.Mutex
	gw      *gateway // non-nil: this PC shares its internet
	useGW   bool     // this PC routes its internet through seenGW
	usingIP [4]byte  // gateway the routes point at
	seenGW  gwSeen

	lastAck   atomic.Int64 // unix nanos of the board's last keepalive answer
	rejoins   atomic.Uint64

	pace *pacer // IP packets towards the board, at the radio's pace

	proxy    *gwProxy // gateway: HTTP proxy on our LoRa address
	useRoute bool     // client: full default route (else system proxy)

	ownMu    sync.Mutex
	ownAt    time.Time
	ownCache bool
	stopped  chan struct{}
	wg       sync.WaitGroup
}

func isElevated() bool { return windows.GetCurrentProcessToken().IsElevated() }

// relaunchElevated starts this exe again through the UAC prompt.
func relaunchElevated(args string) error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	verb, _ := windows.UTF16PtrFromString("runas")
	file, _ := windows.UTF16PtrFromString(exe)
	params, _ := windows.UTF16PtrFromString(args)
	dir, _ := windows.UTF16PtrFromString(filepath.Dir(exe))
	return windows.ShellExecute(0, verb, file, params, dir, windows.SW_SHOWNORMAL)
}

func ensureWintun() error {
	if len(wintunDLL) == 0 {
		return errors.New("this build of Voyager Link has no Wintun driver")
	}
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	p := filepath.Join(filepath.Dir(exe), "wintun.dll")
	if b, err := os.ReadFile(p); err == nil && bytes.Equal(b, wintunDLL) {
		return nil
	}
	if err := os.WriteFile(p, wintunDLL, 0o644); err != nil {
		return fmt.Errorf("cannot write %s: %v", p, err)
	}
	return nil
}

// runHidden runs a console tool (netsh) without flashing a window.
func runHidden(name string, args ...string) (string, error) {
	cmd := exec.Command(name, args...)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	out, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

func configureAdapter(name string, ip [4]byte) error {
	addr := ipString(ip)
	var lastErr error
	// The adapter can take a moment to appear to netsh after creation.
	for i := 0; i < 20; i++ {
		out, err := runHidden("netsh", "interface", "ipv4", "set", "address", "name="+name,
			"source=static", "address="+addr, "mask=255.255.0.0")
		if err == nil {
			lastErr = nil
			break
		}
		lastErr = fmt.Errorf("netsh set address: %v: %s", err, out)
		time.Sleep(500 * time.Millisecond)
	}
	if lastErr != nil {
		return lastErr
	}
	if out, err := runHidden("netsh", "interface", "ipv4", "set", "subinterface", name,
		fmt.Sprintf("mtu=%d", netAdapterMTU), "store=active"); err != nil {
		log.Printf("netsh mtu: %v: %s", err, out) // not fatal: the MSS clamp keeps TCP small
	}
	// Let peers ping this PC (only from the LoRa network). Other inbound services
	// (ssh, web servers...) need their own firewall rule, on purpose.
	const rule = "Voyager LoRa ping"
	if _, err := runHidden("netsh", "advfirewall", "firewall", "show", "rule", "name="+rule); err != nil {
		if out, err := runHidden("netsh", "advfirewall", "firewall", "add", "rule", "name="+rule,
			"dir=in", "action=allow", "protocol=icmpv4:8,any", "remoteip=10.73.0.0/16"); err != nil {
			log.Printf("firewall rule: %v: %s", err, out)
		}
	}
	return nil
}

// netHandshake asks the board for network mode and returns its address.
func netHandshake(p serial.Port) (netStartInfo, error) {
	var sc improvScanner
	buf := make([]byte, 512)
	deadline := time.Now().Add(8 * time.Second)
	nextSend := time.Now()
	for time.Now().Before(deadline) {
		if time.Now().After(nextSend) { // resend: the board may still be booting
			// A board left in network mode (the previous Voyager Link was killed
			// without "leave") reads USB as SLIP and would ignore the Improv command:
			// send "leave" first. An idle board's Improv parser skips it.
			if _, err := p.Write(slipEncode([]byte{0, netCtlLeave})); err != nil {
				return netStartInfo{}, err
			}
			time.Sleep(150 * time.Millisecond)
			if _, err := p.Write(improvNetStart()); err != nil {
				return netStartInfo{}, err
			}
			nextSend = time.Now().Add(1500 * time.Millisecond)
		}
		n, err := p.Read(buf)
		if err != nil {
			return netStartInfo{}, err
		}
		chunk := buf[:n]
		for {
			typ, data, ok := sc.feed(chunk)
			chunk = nil
			if !ok {
				break
			}
			switch {
			case typ == improvTypeRPCResult && len(data) > 0 && data[0] == improvCmdNetStart:
				return parseNetStart(data)
			case typ == improvTypeError && len(data) > 0 && data[0] != 0:
				return netStartInfo{}, errImprovRefused
			}
		}
	}
	return netStartInfo{}, errors.New("the board did not answer: update its firmware (needs LoRa network support)")
}

// netJoin starts the bridge on portName. Needs an elevated process.
func (s *server) netJoin(portName string) error {
	s.net.mu.Lock()
	defer s.net.mu.Unlock()
	if s.net.bridge != nil {
		return nil
	}
	err := s.netJoinLocked(portName)
	if err != nil {
		s.net.lastErr = err.Error()
	} else {
		s.net.lastErr = ""
	}
	return err
}

func (s *server) netJoinLocked(portName string) error {
	if !isElevated() {
		return errNeedsAdmin
	}
	owner := &session{srv: s, origin: netOwnerOrigin}
	if err := s.claim(portName, owner); err != nil {
		return err
	}
	b := &netBridge{srv: s, owner: owner, portName: portName, stopped: make(chan struct{})}
	fail := func(err error) error {
		b.closeResources()
		return err
	}

	mode := &serial.Mode{BaudRate: 115200, DataBits: 8, Parity: serial.NoParity, StopBits: serial.OneStopBit,
		InitialStatusBits: &serial.ModemOutputBits{DTR: false, RTS: false}}
	p, err := serial.Open(portName, mode)
	if err != nil {
		return fail(fmt.Errorf("cannot open %s: %v", portName, err))
	}
	b.port = p
	p.SetDTR(false)
	p.SetRTS(false)
	p.SetReadTimeout(100 * time.Millisecond)
	p.ResetInputBuffer()

	if b.info, err = netHandshake(p); err != nil {
		return fail(err)
	}
	if err := ensureWintun(); err != nil {
		return fail(err)
	}
	tun.WintunTunnelType = "Voyager"
	dev, err := tun.CreateTUN(netAdapterName, netAdapterMTU)
	if err != nil {
		return fail(fmt.Errorf("cannot create the network adapter: %v", err))
	}
	b.dev = dev
	name, err := dev.Name()
	if err != nil {
		name = netAdapterName
	}
	if err := configureAdapter(name, b.info.IP); err != nil {
		return fail(err)
	}

	b.pace = newPacer(b.info.AirFullMs, b.info.AirSmallMs, b.writeSerial)
	go b.pace.run()
	b.lastAck.Store(time.Now().UnixNano())
	b.wg.Add(3)
	go b.tunToSerial()
	go b.serialToTun()
	go b.keepalive()
	s.net.bridge = b
	log.Printf("LoRa network: joined as %s (node %s) on %s, adapter %s", ipString(b.info.IP), b.info.Node, portName, name)
	return nil
}

func (s *server) netLeave(why string) {
	s.net.mu.Lock()
	b := s.net.bridge
	s.net.bridge = nil
	if why != "" && why != "left" {
		s.net.lastErr = why
	}
	s.net.mu.Unlock()
	if b != nil {
		b.stop(why)
	}
}

func (b *netBridge) writeSerial(p []byte) error {
	b.wmu.Lock()
	defer b.wmu.Unlock()
	_, err := b.port.Write(p)
	return err
}

func (b *netBridge) tunToSerial() {
	defer b.wg.Done()
	bs := b.dev.BatchSize()
	bufs := make([][]byte, bs)
	for i := range bufs {
		bufs[i] = make([]byte, 2048)
	}
	sizes := make([]int, bs)
	tooBigLogged := time.Time{}
	for {
		n, err := b.dev.Read(bufs, sizes, 0)
		if err != nil {
			b.fail("adapter closed: " + err.Error())
			return
		}
		for i := 0; i < n; i++ {
			pkt := bufs[i][:sizes[i]]
			act, why := classifyOutbound(pkt, b.info.IP, b.gatewayForClient())
			if act == outDrop {
				b.c.drop.Add(1)
				if strings.HasPrefix(why, "too big") && time.Since(tooBigLogged) > time.Minute {
					log.Printf("LoRa network: dropped a packet, %s (only TCP is resized to fit)", why)
					tooBigLogged = time.Now()
				}
				continue
			}
			clampMSS(pkt, netClampMSS)
			frame := pkt
			if act == outViaGW {
				b.shareMu.Lock()
				frame = sendToFrame(nodeOf(b.usingIP), pkt)
				b.shareMu.Unlock()
			}
			b.pace.push(pkt, slipEncode(frame))
			b.c.tx.Add(1)
		}
	}
}

func (b *netBridge) serialToTun() {
	defer b.wg.Done()
	dec := newSlipDecoder(netAdapterMTU + 24)
	buf := make([]byte, 4096)
	for {
		select {
		case <-b.stopped:
			return
		default:
		}
		n, err := b.port.Read(buf)
		if err != nil {
			b.fail("board disconnected: " + err.Error())
			return
		}
		for _, f := range dec.feed(buf[:n]) {
			if len(f) == 2 && f[0] == 0 && f[1] == netCtlAliveAck {
				b.lastAck.Store(time.Now().UnixNano())
				continue
			}
			if len(f) == 3 && f[0] == 0 && f[1] == netCtlBusy {
				b.pace.backoff() // the radio queue is filling: share the channel
				continue
			}
			if !ipv4OK(f) {
				b.c.drop.Add(1)
				continue
			}
			if gwIP, ok := parseGwBeacon(f); ok {
				if gwIP != b.info.IP {
					b.seenGW.set(gwIP)
				}
				continue
			}
			// Sent to us but addressed to the internet: a peer using our shared internet.
			if f[16] != netPrefixA || f[17] != netPrefixB {
				b.shareMu.Lock()
				gw := b.gw
				b.shareMu.Unlock()
				if gw != nil {
					b.c.rx.Add(1)
					gw.inject(f)
				} else {
					b.c.drop.Add(1)
				}
				continue
			}
			clampMSS(f, netClampMSS)
			if _, err := b.dev.Write([][]byte{f}, 0); err != nil {
				b.c.drop.Add(1)
				continue
			}
			b.c.rx.Add(1)
		}
	}
}

func (b *netBridge) keepalive() {
	defer b.wg.Done()
	t := time.NewTicker(5 * time.Second)
	defer t.Stop()
	beat := 0
	for {
		select {
		case <-b.stopped:
			return
		case <-t.C:
			if err := b.writeSerial(slipEncode([]byte{0, netCtlKeepalive})); err != nil {
				b.fail("serial write failed: " + err.Error())
				return
			}
			if time.Since(time.Unix(0, b.lastAck.Load())) > netAckTimeout {
				// The board stopped answering: it rebooted into idle mode. Ask again;
				// its Improv answer is skipped by the SLIP decoder as a bad frame.
				b.writeSerial(improvNetStart())
				b.lastAck.Store(time.Now().UnixNano()) // retry every netAckTimeout
				b.rejoins.Add(1)
				log.Printf("LoRa network: board not answering, re-entering network mode")
			}
			beat++
			b.shareMu.Lock()
			sharing := b.gw != nil
			b.shareMu.Unlock()
			if sharing && beat%6 == 1 { // every 30 s, and right after sharing starts
				b.writeSerial(slipEncode(gwBeacon(b.info.IP)))
			}
		}
	}
}

// fail stops the bridge from inside one of its goroutines.
func (b *netBridge) fail(why string) {
	select {
	case <-b.stopped:
		return // already stopping: errors from closing are expected
	default:
	}
	go b.srv.netLeave(why)
}

func (b *netBridge) stop(why string) {
	b.stopOnce.Do(func() {
		close(b.stopped)
		b.setUse(false)
		b.setShare(false)
		if b.pace != nil {
			b.pace.close()
		}
		if b.port != nil {
			b.writeSerial(slipEncode([]byte{0, netCtlLeave})) // board back to idle at once
		}
		b.closeResources()
		b.wg.Wait()
		log.Printf("LoRa network: left (%s)", why)
	})
}

// ownInternet: does this PC have a default route other than the LoRa one? Then
// "use shared internet" would replace a fast connection with a ~1 KB/s one.
// Cached for 30 s (it runs a tool).
func (b *netBridge) ownInternet() bool {
	b.ownMu.Lock()
	defer b.ownMu.Unlock()
	if time.Since(b.ownAt) < 30*time.Second {
		return b.ownCache
	}
	b.ownAt = time.Now()
	out, err := runHidden("route", "print", "-4", "0.0.0.0")
	if err != nil {
		return b.ownCache
	}
	self := ipString(b.info.IP)
	own := false
	for _, line := range strings.Split(out, "\n") {
		f := strings.Fields(line)
		// "0.0.0.0  0.0.0.0  <gateway>  <interface ip>  <metric>"
		if len(f) == 5 && f[0] == "0.0.0.0" && f[1] == "0.0.0.0" && f[3] != self {
			own = true
		}
	}
	b.ownCache = own
	return own
}

// gatewayForClient: the gateway to send internet traffic to, nil if not in use.
func (b *netBridge) gatewayForClient() *[4]byte {
	b.shareMu.Lock()
	defer b.shareMu.Unlock()
	if !b.useGW || !b.useRoute {
		return nil
	}
	ip := b.usingIP
	return &ip
}

// setShare turns this PC into the LoRa network's internet gateway (or back).
func (b *netBridge) setShare(on bool) error {
	b.shareMu.Lock()
	defer b.shareMu.Unlock()
	if on == (b.gw != nil) {
		return nil
	}
	if !on {
		if b.proxy != nil {
			b.proxy.close()
			b.proxy = nil
		}
		b.gw.close()
		b.gw = nil
		log.Printf("LoRa network: stopped sharing internet")
		return nil
	}
	if b.useGW {
		return errors.New("this PC uses another gateway's internet; stop that first")
	}
	if b.info.Proto != "2" {
		return errors.New("the board's firmware is too old for internet sharing: update it")
	}
	gw, err := newGateway(func(p []byte) {
		// Replies for peers: their 10.73 address routes them, like any packet.
		if len(p) <= netMaxIP {
			b.pace.push(p, slipEncode(p))
			b.c.tx.Add(1)
		} else {
			b.c.drop.Add(1)
		}
	})
	if err != nil {
		return err
	}
	// Proxy for clients in proxy mode (the default), on our LoRa address only.
	proxy, err := newGwProxy(fmt.Sprintf("%s:%d", ipString(b.info.IP), gwProxyPort))
	if err != nil {
		gw.close()
		return fmt.Errorf("cannot start the gateway proxy: %v", err)
	}
	const rule = "Voyager LoRa gateway proxy"
	if _, err := runHidden("netsh", "advfirewall", "firewall", "show", "rule", "name="+rule); err != nil {
		if out, err := runHidden("netsh", "advfirewall", "firewall", "add", "rule", "name="+rule,
			"dir=in", "action=allow", "protocol=TCP", fmt.Sprintf("localport=%d", gwProxyPort),
			"remoteip=10.73.0.0/16"); err != nil {
			log.Printf("firewall rule: %v: %s", err, out)
		}
	}
	b.gw, b.proxy = gw, proxy
	log.Printf("LoRa network: sharing this PC's internet as gateway %s (proxy %s)", ipString(b.info.IP), proxy.Addr())
	return nil
}

// setUse routes this PC's internet through the gateway heard on the network (or stops).
func (b *netBridge) setUse(on bool) error {
	b.shareMu.Lock()
	defer b.shareMu.Unlock()
	if on == b.useGW {
		return nil
	}
	name, _ := b.dev.Name()
	if !on {
		b.useGW = false
		gw := ipString(b.usingIP)
		if b.useRoute {
			runHidden("netsh", "interface", "ipv4", "delete", "route", "prefix=0.0.0.0/0", "interface="+name, "nexthop="+gw)
			runHidden("netsh", "interface", "ipv4", "delete", "dnsservers", "name="+name, "address=all", "validate=no")
			runHidden("netsh", "interface", "ipv4", "set", "interface", name, "metric=automatic")
		} else {
			restoreSystemProxy()
		}
		log.Printf("LoRa network: stopped using shared internet via %s", gw)
		return nil
	}
	if b.gw != nil {
		return errors.New("this PC is sharing its own internet")
	}
	if b.info.Proto != "2" {
		return errors.New("the board's firmware is too old for shared internet: update it")
	}
	ip, ok := b.seenGW.get()
	if !ok {
		return errors.New("no gateway heard on the LoRa network")
	}
	gw := ipString(ip)
	if !b.srv.netUseRoute {
		// Proxy mode: only apps that use the system proxy go over LoRa.
		if err := setSystemProxy(fmt.Sprintf("%s:%d", gw, gwProxyPort)); err != nil {
			return err
		}
		b.usingIP, b.useGW, b.useRoute = ip, true, false
		log.Printf("LoRa network: using shared internet via %s (system proxy %s:%d)", gw, gw, gwProxyPort)
		return nil
	}
	// Route mode (-use-internet-route): everything, including background traffic.
	steps := [][]string{
		{"interface", "ipv4", "set", "interface", name, "metric=1"},
		{"interface", "ipv4", "add", "route", "prefix=0.0.0.0/0", "interface=" + name, "nexthop=" + gw, "metric=1", "store=active"},
		{"interface", "ipv4", "set", "dnsservers", "name=" + name, "source=static", "address=1.1.1.1", "register=none", "validate=no"},
		{"interface", "ipv4", "add", "dnsservers", "name=" + name, "address=8.8.8.8", "index=2", "validate=no"},
	}
	for _, s := range steps {
		if out, err := runHidden("netsh", s...); err != nil {
			log.Printf("netsh %v: %v: %s", s, err, out)
			if s[2] == "add" && s[3] == "route" {
				return fmt.Errorf("cannot add the route through %s: %s", gw, out)
			}
		}
	}
	b.usingIP, b.useGW, b.useRoute = ip, true, true
	log.Printf("LoRa network: using shared internet via %s (default route)", gw)
	return nil
}

func (s *server) netSetShare(on bool) error {
	s.net.mu.Lock()
	b := s.net.bridge
	s.net.mu.Unlock()
	if b == nil {
		return errors.New("join the LoRa network first")
	}
	return b.setShare(on)
}

func (s *server) netSetUse(on bool) error {
	s.net.mu.Lock()
	b := s.net.bridge
	s.net.mu.Unlock()
	if b == nil {
		return errors.New("join the LoRa network first")
	}
	return b.setUse(on)
}

func (b *netBridge) closeResources() {
	if b.dev != nil {
		b.dev.Close()
	}
	if b.port != nil {
		b.port.Close()
	}
	b.srv.release(b.portName, b.owner)
}
