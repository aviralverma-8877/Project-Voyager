//go:build !windows

package main

import (
	"errors"
	"sync"
)

// netBridge exists on every OS so the shared code compiles; only Windows runs it.
type netBridge struct {
	portName string
	info     netStartInfo
	c        netCounters
	shareMu  sync.Mutex
	gw       *gateway
	useGW    bool
	seenGW   gwSeen
	pace     *pacer
	useRoute bool
}

func (b *netBridge) ownInternet() bool { return false }

func (s *server) netSetShare(on bool) error { return errors.New("not supported on this OS") }
func (s *server) netSetUse(on bool) error   { return errors.New("not supported on this OS") }

func recoverStaleProxy() {}

func isElevated() bool                   { return false }
func relaunchElevated(args string) error { return errors.New("not supported on this OS") }

func (s *server) netJoin(portName string) error {
	return errors.New("the LoRa network is only available on Windows for now")
}

func (s *server) netLeave(why string) {}
