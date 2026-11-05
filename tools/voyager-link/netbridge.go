package main

// LoRa network bridge, OS-independent part: state shared with the server and tray.
// The Windows adapter work is in netbridge_windows.go.

import (
	"errors"
	"sync"
	"sync/atomic"
	"time"
)

const netOwnerOrigin = "voyager-link://lora-network"

var errNeedsAdmin = errors.New("joining the LoRa network needs administrator rights")

type netStatus struct {
	Active bool   `json:"active"`
	IP     string `json:"ip,omitempty"`
	Port   string `json:"port,omitempty"`
	TX     uint64 `json:"tx"`
	RX     uint64 `json:"rx"`
	Drop   uint64 `json:"drop"`
	Error  string `json:"error,omitempty"`

	// Internet sharing
	Sharing    bool   `json:"sharing"`            // this PC is a gateway
	Gateway    string `json:"gateway,omitempty"`  // a gateway heard recently (client side)
	UsingGW    bool   `json:"using_gateway"`      // this PC routes its internet through it
	UseMode    string `json:"use_mode,omitempty"` // "proxy" (system proxy) or "route"
	ShareFlows uint64 `json:"share_flows,omitempty"`

	// Pacing towards the radio (netpace.go)
	Queued int32  `json:"queued"`
	Shaped uint64 `json:"shaped_drops"`
	OwnNet bool   `json:"own_internet"` // this PC has another default route
}

// A gateway is "available" while its beacons (every 30 s) keep coming.
const gwStaleAfter = 100 * time.Second

// gwSeen tracks the gateway announced on the LoRa network (the latest one wins).
type gwSeen struct {
	mu sync.Mutex
	ip [4]byte
	at time.Time
}

func (g *gwSeen) set(ip [4]byte) {
	g.mu.Lock()
	g.ip, g.at = ip, time.Now()
	g.mu.Unlock()
}

func (g *gwSeen) get() ([4]byte, bool) {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.ip, !g.at.IsZero() && time.Since(g.at) < gwStaleAfter
}

// netState is the one bridge this process may run (one PC = one network node).
type netState struct {
	mu      sync.Mutex
	bridge  *netBridge
	lastErr string
}

type netCounters struct{ tx, rx, drop atomic.Uint64 }

func (s *server) netStatus() netStatus {
	s.net.mu.Lock()
	b, lastErr := s.net.bridge, s.net.lastErr
	s.net.mu.Unlock()
	if b == nil {
		return netStatus{Error: lastErr}
	}
	st := netStatus{
		Active: true,
		IP:     ipString(b.info.IP),
		Port:   b.portName,
		TX:     b.c.tx.Load(),
		RX:     b.c.rx.Load(),
		Drop:   b.c.drop.Load(),
	}
	if b.pace != nil {
		st.Queued = b.pace.Queued.Load()
		st.Shaped = b.pace.Dropped.Load()
	}
	st.OwnNet = b.ownInternet()
	b.shareMu.Lock()
	if b.gw != nil {
		st.Sharing = true
		st.ShareFlows = b.gw.TCP.Load() + b.gw.UDP.Load()
	}
	st.UsingGW = b.useGW
	if b.useGW {
		st.UseMode = "proxy"
		if b.useRoute {
			st.UseMode = "route"
		}
	}
	b.shareMu.Unlock()
	if ip, ok := b.seenGW.get(); ok {
		st.Gateway = ipString(ip)
	}
	return st
}

// pickNetPort chooses the board for the network: a likely ESP32 port no page holds.
func (s *server) pickNetPort() (string, error) {
	ports, err := listPorts()
	if err != nil {
		return "", err
	}
	var busy string
	for pass := 0; pass < 2; pass++ {
		for _, p := range ports {
			if pass == 0 && !p.Likely || pass == 1 && !p.IsUSB {
				continue
			}
			s.mu.Lock()
			o := s.owners[portKey(p.Name)]
			s.mu.Unlock()
			if o != nil {
				busy = p.Name + " is in use by " + ownerName(o)
				continue
			}
			return p.Name, nil
		}
	}
	if busy != "" {
		return "", errors.New(busy + "; disconnect it there first")
	}
	return "", errors.New("no Voyager board found: plug one in over USB")
}

// ownerName describes who holds a port, for error messages.
func ownerName(o *session) string {
	if o.origin == netOwnerOrigin {
		return "the LoRa network"
	}
	return "the page from " + hostOf(o.origin)
}
