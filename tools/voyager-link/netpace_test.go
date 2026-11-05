package main

import (
	"encoding/binary"
	"testing"
	"time"
)

func tcpData(n int, flags byte) []byte {
	tcp := make([]byte, 20+n)
	tcp[12] = 5 << 4
	tcp[13] = flags
	return ipv4Packet(peer, 6, tcp)
}

func udpTo(port uint16, n int) []byte {
	u := make([]byte, 8+n)
	binary.BigEndian.PutUint16(u[2:4], port)
	return ipv4Packet(peer, 17, u)
}

func TestInteractiveClassification(t *testing.T) {
	cases := []struct {
		name string
		p    []byte
		want bool
	}{
		{"icmp", ipv4Packet(peer, 1, make([]byte, 36)), true},
		{"dns", udpTo(53, 40), true},
		{"quic/other udp", udpTo(443, 100), false},
		{"tcp syn", tcpData(0, 0x02), true},
		{"tcp fin", tcpData(100, 0x11), true},
		{"pure ack", tcpData(0, 0x10), true},
		{"keystrokes", tcpData(48, 0x18), true},
		{"bulk segment", tcpData(190, 0x10), false},
	}
	for _, c := range cases {
		if got := interactive(c.p); got != c.want {
			t.Errorf("%s: got %v", c.name, got)
		}
	}
}

func TestPacerCostFromBoardAirtimes(t *testing.T) {
	p := newPacer(240, 30, nil)
	small, full := p.cost(20), p.cost(netMaxIP)
	if small != 30*time.Millisecond+30*time.Millisecond+pacerOverhead {
		t.Fatalf("small %v", small)
	}
	if full != 240*time.Millisecond+30*time.Millisecond+pacerOverhead {
		t.Fatalf("full %v", full)
	}
	if mid := p.cost(135); mid <= small || mid >= full {
		t.Fatalf("mid %v not between", mid)
	}
	// Unknown airtimes fall back to conservative values.
	if q := newPacer(0, 0, nil); q.airFull != 400*time.Millisecond {
		t.Fatal("no fallback")
	}
}

func TestPacerQueuePolicy(t *testing.T) {
	p := newPacer(240, 30, nil)
	bulk := tcpData(190, 0x10)
	ack := tcpData(0, 0x10)
	for i := 0; i < pacerMaxQueued; i++ {
		p.push(bulk, []byte{byte(i)})
	}
	p.push(bulk, []byte{99}) // full: bulk is tail-dropped
	if p.Dropped.Load() != 1 {
		t.Fatalf("dropped %d", p.Dropped.Load())
	}
	p.push(ack, []byte{200}) // interactive evicts the oldest bulk
	if p.Dropped.Load() != 2 {
		t.Fatalf("dropped %d", p.Dropped.Load())
	}
	pk, _ := p.pop()
	if pk.frame[0] != 200 {
		t.Fatal("interactive packet must go first")
	}
	pk, _ = p.pop()
	if pk.frame[0] != 1 { // bulk 0 was evicted
		t.Fatalf("next bulk %d", pk.frame[0])
	}
}

func TestPacerDropsStalePackets(t *testing.T) {
	p := newPacer(240, 30, nil)
	clock := time.Now()
	p.now = func() time.Time { return clock }
	p.push(tcpData(190, 0x10), []byte{1})
	p.push(tcpData(190, 0x10), []byte{2})
	clock = clock.Add(pacerMaxAge + time.Second)
	p.push(tcpData(190, 0x10), []byte{3})
	pk, ok := p.pop()
	if !ok || pk.frame[0] != 3 || p.Dropped.Load() != 2 {
		t.Fatalf("got %v, dropped %d", pk.frame, p.Dropped.Load())
	}
}

// The real loop: a burst far above link capacity comes out at the radio's pace,
// with the queue bounded (no 20-deep board queue overflow anymore).
func TestPacerRunPacesABurst(t *testing.T) {
	var sent []time.Time
	done := make(chan struct{})
	p := newPacer(20, 5, func(b []byte) error {
		sent = append(sent, time.Now())
		if len(sent) == 5 {
			close(done)
		}
		return nil
	})
	go p.run()
	for i := 0; i < 50; i++ { // Windows-style burst
		p.push(tcpData(190, 0x10), []byte{byte(i)})
	}
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("nothing sent")
	}
	p.close()
	// 50 offered: at most one already in flight plus a full queue survive.
	if p.Dropped.Load() < 50-pacerMaxQueued-1 {
		t.Fatalf("queue not bounded: only %d dropped", p.Dropped.Load())
	}
	gap := sent[4].Sub(sent[3])
	if gap < p.cost(netMaxIP)-5*time.Millisecond {
		t.Fatalf("packets released %v apart, want >= %v", gap, p.cost(netMaxIP))
	}
}
