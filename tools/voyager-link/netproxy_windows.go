//go:build windows

package main

// Client side of proxy mode: point the Windows (WinINET) proxy setting at the
// gateway, and put the previous setting back afterwards. Browsers and most apps
// follow it; background services (Windows Update via WinHTTP, etc.) don't, which
// is the point: they must not eat the LoRa link.
//
// The previous setting is also saved to disk, so a crash or kill can't leave the
// browser pointing at a LoRa gateway: the next start restores it.

import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"

	"golang.org/x/sys/windows/registry"
)

const inetSettingsKey = `Software\Microsoft\Windows\CurrentVersion\Internet Settings`

var (
	wininet               = syscall.NewLazyDLL("wininet.dll")
	procInternetSetOption = wininet.NewProc("InternetSetOptionW")
)

type savedProxy struct {
	Enable      uint64 `json:"enable"`
	HadServer   bool   `json:"had_server"`
	Server      string `json:"server"`
	HadOverride bool   `json:"had_override"`
	Override    string `json:"override"`
	Ours        string `json:"ours"` // the value we set, to recognise a leftover
}

var (
	proxyMu    sync.Mutex
	proxySaved *savedProxy
)

func proxyBackupPath() string { return filepath.Join(logDir(), "proxy-backup.json") }

// refreshProxy tells running apps that the proxy setting changed.
func refreshProxy() {
	const optSettingsChanged, optRefresh = 39, 37
	procInternetSetOption.Call(0, optSettingsChanged, 0, 0)
	procInternetSetOption.Call(0, optRefresh, 0, 0)
}

func setSystemProxy(hostPort string) error {
	proxyMu.Lock()
	defer proxyMu.Unlock()
	k, err := registry.OpenKey(registry.CURRENT_USER, inetSettingsKey, registry.QUERY_VALUE|registry.SET_VALUE)
	if err != nil {
		return fmt.Errorf("proxy settings: %v", err)
	}
	defer k.Close()
	if proxySaved == nil { // keep the user's own setting from before the first change
		s := &savedProxy{Ours: hostPort}
		s.Enable, _, _ = k.GetIntegerValue("ProxyEnable")
		if v, _, err := k.GetStringValue("ProxyServer"); err == nil {
			s.HadServer, s.Server = true, v
		}
		if v, _, err := k.GetStringValue("ProxyOverride"); err == nil {
			s.HadOverride, s.Override = true, v
		}
		if b, err := json.Marshal(s); err == nil {
			os.MkdirAll(logDir(), 0o755)
			if err := os.WriteFile(proxyBackupPath(), b, 0o600); err != nil {
				return fmt.Errorf("cannot save the current proxy setting: %v", err)
			}
		}
		proxySaved = s
	}
	if err := k.SetStringValue("ProxyServer", hostPort); err != nil {
		return err
	}
	// Local and LoRa-network addresses stay direct.
	k.SetStringValue("ProxyOverride", "<local>;10.73.*;localhost;127.*")
	if err := k.SetDWordValue("ProxyEnable", 1); err != nil {
		return err
	}
	refreshProxy()
	return nil
}

func applySavedProxy(s *savedProxy) {
	k, err := registry.OpenKey(registry.CURRENT_USER, inetSettingsKey, registry.SET_VALUE)
	if err != nil {
		return
	}
	defer k.Close()
	k.SetDWordValue("ProxyEnable", uint32(s.Enable))
	if s.HadServer {
		k.SetStringValue("ProxyServer", s.Server)
	} else {
		k.DeleteValue("ProxyServer")
	}
	if s.HadOverride {
		k.SetStringValue("ProxyOverride", s.Override)
	} else {
		k.DeleteValue("ProxyOverride")
	}
	refreshProxy()
}

func restoreSystemProxy() {
	proxyMu.Lock()
	defer proxyMu.Unlock()
	if proxySaved == nil {
		return
	}
	applySavedProxy(proxySaved)
	proxySaved = nil
	os.Remove(proxyBackupPath())
}

// recoverStaleProxy runs at start: a previous Voyager Link that died while using
// shared internet left the system proxy on a LoRa gateway. Put the user's back,
// unless they changed it to something else since.
func recoverStaleProxy() {
	b, err := os.ReadFile(proxyBackupPath())
	if err != nil {
		return
	}
	var s savedProxy
	if json.Unmarshal(b, &s) == nil {
		k, err := registry.OpenKey(registry.CURRENT_USER, inetSettingsKey, registry.QUERY_VALUE)
		if err == nil {
			cur, _, _ := k.GetStringValue("ProxyServer")
			k.Close()
			if cur == s.Ours || strings.HasPrefix(cur, "10.73.") {
				applySavedProxy(&s)
				log.Printf("restored the system proxy setting left by a previous run")
			}
		}
	}
	os.Remove(proxyBackupPath())
}
