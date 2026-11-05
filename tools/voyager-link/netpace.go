package main

// Pacing towards the board: the LoRa link carries a few frames per second, while
// Windows happily offers tens of packets per second (updates, sync, telemetry).
// Handing them all to the board just overflows its 20-slot radio queue, which then
// drops almost everything, so every connection stalls (seen on hardware: 5,732 of
// 6,374 packets dropped, 2.6 s ping).
//
// The pacer behaves like a router on a slow link:
//   - it releases one packet per estimated radio time (frame airtime + ACK +
//     channel access), using airtimes the board reports for its radio config;
//   - its queue is short (pacerMaxQueued, pacerMaxAge), so drops happen early and
//     TCP's congestion control slows down instead of waiting on a growing queue;
//   - small "interactive" packets (DNS, ICMP, TCP handshakes, ACKs and keystroke-
//     sized segments) jump ahead of bulk data and are dropped last.

import (
	"encoding/binary"
	"sync"
	"sync/atomic"
	"time"
)

const (
	pacerMaxQueued = 10
	pacerMaxAge    = 3 * time.Second
	pacerOverhead  = 60 * time.Millisecond // listen-before-talk, USB, board processing
	pacerSmallLen  = 24                    // payload length the board's "small" airtime is for
)

// interactive reports whether a packet should go before bulk data.
func interactive(p []byte) bool {
	if len(p) < 20 {
		return true
	}
	ihl := int(p[0]&0x0F) * 4
	switch p[9] {
	case 1: // ICMP
		return true
	case 17: // UDP: DNS
		if len(p) >= ihl+4 {
			sp, dp := binary.BigEndian.Uint16(p[ihl:]), binary.BigEndian.Uint16(p[ihl+2:])
			return sp == 53 || dp == 53
		}
		return false
	case 6: // TCP: control or small segments
		if len(p) < ihl+20 {
			return true
		}
		flags := p[ihl+13]
		if flags&0x07 != 0 { // SYN, FIN, RST
			return true
		}
		doff := int(p[ihl+12]>>4) * 4
		return len(p)-ihl-doff <= 64 // pure ACK or a few bytes (ssh keystrokes, chat)
	}
	return false
}

type pacedPkt struct {
	frame  []byte // what goes on the serial line (SLIP-encoded)
	airLen int    // IP packet length: what goes on the air
	at     time.Time
}

type pacer struct {
	mu     sync.Mutex
	cond   *sync.Cond
	hi, lo []pacedPkt
	closed bool

	airFull, airSmall time.Duration // board-reported airtime of a full / minimal frame
	write             func([]byte) error

	Queued  atomic.Int32
	Dropped atomic.Uint64
	now     func() time.Time

	// Slow-down factor in permille (1000 = the radio's nominal pace). The board
	// reports a filling radio queue (00 82) when other nodes share the channel;
	// each report multiplies the pace by 1.5 (max 4x), quiet periods recover 5%
	// per packet sent.
	factor   atomic.Int64
	lastBusy atomic.Int64 // unix nanos
}

const (
	pacerMaxFactor    = 4000
	pacerRecoverQuiet = 5 * time.Second
)

func (p *pacer) backoff() {
	for {
		f := p.factor.Load()
		n := f * 3 / 2
		if n > pacerMaxFactor {
			n = pacerMaxFactor
		}
		if p.factor.CompareAndSwap(f, n) {
			p.lastBusy.Store(p.now().UnixNano())
			return
		}
	}
}

func (p *pacer) recover() {
	for {
		f := p.factor.Load()
		n := f * 95 / 100
		if n < 1000 {
			n = 1000
		}
		if p.factor.CompareAndSwap(f, n) {
			return
		}
	}
}

func newPacer(airFullMs, airSmallMs int, write func([]byte) error) *pacer {
	if airFullMs <= 0 || airSmallMs <= 0 || airSmallMs > airFullMs {
		airFullMs, airSmallMs = 400, 60 // unknown radio config: be conservative
	}
	p := &pacer{
		airFull:  time.Duration(airFullMs) * time.Millisecond,
		airSmall: time.Duration(airSmallMs) * time.Millisecond,
		write:    write,
		now:      time.Now,
	}
	p.cond = sync.NewCond(&p.mu)
	p.factor.Store(1000)
	return p
}

// cost: radio time one packet of airLen bytes takes, ACK and channel access included.
func (p *pacer) cost(airLen int) time.Duration {
	span := netMaxIP - pacerSmallLen
	extra := airLen - pacerSmallLen
	if extra < 0 {
		extra = 0
	}
	data := p.airSmall + (p.airFull-p.airSmall)*time.Duration(extra)/time.Duration(span)
	base := data + p.airSmall + pacerOverhead // + the ACK frame (minimal size)
	return base * time.Duration(p.factor.Load()) / 1000
}

// push queues one IP packet (already wrapped as frame). Never blocks.
func (p *pacer) push(ip []byte, frame []byte) {
	pk := pacedPkt{frame: frame, airLen: len(ip), at: p.now()}
	hi := interactive(ip)
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return
	}
	if len(p.hi)+len(p.lo) >= pacerMaxQueued {
		switch {
		case !hi:
			p.Dropped.Add(1) // bulk arriving at a full queue: tail drop
			return
		case len(p.lo) > 0:
			p.lo = p.lo[1:] // make room for interactive traffic
			p.Dropped.Add(1)
		default:
			p.hi = p.hi[1:]
			p.Dropped.Add(1)
		}
	}
	if hi {
		p.hi = append(p.hi, pk)
	} else {
		p.lo = append(p.lo, pk)
	}
	p.Queued.Store(int32(len(p.hi) + len(p.lo)))
	p.cond.Signal()
}

// pop takes the next packet to send: interactive first; stale packets are dropped
// (late data only wastes airtime: TCP has retransmitted it already).
func (p *pacer) pop() (pacedPkt, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	for {
		for !p.closed && len(p.hi)+len(p.lo) == 0 {
			p.cond.Wait()
		}
		if p.closed {
			return pacedPkt{}, false
		}
		var pk pacedPkt
		if len(p.hi) > 0 {
			pk, p.hi = p.hi[0], p.hi[1:]
		} else {
			pk, p.lo = p.lo[0], p.lo[1:]
		}
		p.Queued.Store(int32(len(p.hi) + len(p.lo)))
		if p.now().Sub(pk.at) > pacerMaxAge {
			p.Dropped.Add(1)
			continue
		}
		return pk, true
	}
}

// run sends queued packets at the radio's pace until close.
func (p *pacer) run() {
	for {
		pk, ok := p.pop()
		if !ok {
			return
		}
		if err := p.write(pk.frame); err != nil {
			return
		}
		if p.now().Sub(time.Unix(0, p.lastBusy.Load())) > pacerRecoverQuiet {
			p.recover()
		}
		time.Sleep(p.cost(pk.airLen))
	}
}

func (p *pacer) close() {
	p.mu.Lock()
	p.closed = true
	p.hi, p.lo = nil, nil
	p.mu.Unlock()
	p.cond.Broadcast()
}
