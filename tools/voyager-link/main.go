// Voyager Link: tray helper that lets the device-served Voyager web page (plain http,
// so no Web Serial in Chrome) reach the board's COM port through ws://127.0.0.1.
// See server.go for the protocol and README.md for usage.
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"fyne.io/systray"
)

var version = "dev" // set with -ldflags "-X main.version=1.0.0"

func logDir() string {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		base = os.TempDir()
	}
	return filepath.Join(base, "VoyagerLink")
}

func setupLog(console bool) {
	dir := logDir()
	_ = os.MkdirAll(dir, 0o755)
	path := filepath.Join(dir, "voyager-link.log")
	// Keep one previous log; a fresh file per start keeps it small.
	_ = os.Rename(path, path+".old")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	var w io.Writer = io.Discard
	if err == nil {
		w = f
	}
	if console {
		if err == nil {
			w = io.MultiWriter(f, os.Stderr)
		} else {
			w = os.Stderr
		}
	}
	log.SetOutput(w)
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
}

// joinNetwork picks the board and starts the LoRa network bridge (errors land in
// the tray status via netStatus).
func joinNetwork(srv *server) { joinNetworkWith(srv, false, false) }

// joinNetworkWith joins, then optionally shares this PC's internet or waits (up to
// 3 minutes) for a gateway beacon and uses it.
func joinNetworkWith(srv *server, share, use bool) {
	joinNetworkOnly(srv)
	if srv.netStatus().Active && share {
		if err := srv.netSetShare(true); err != nil {
			log.Printf("share internet: %v", err)
		}
	}
	if use {
		for i := 0; i < 90 && srv.netStatus().Active; i++ {
			if srv.netStatus().Gateway != "" {
				if err := srv.netSetUse(true); err != nil {
					log.Printf("use shared internet: %v", err)
				}
				return
			}
			time.Sleep(2 * time.Second)
		}
		log.Printf("use shared internet: no gateway heard")
	}
}

func joinNetworkOnly(srv *server) {
	port, err := srv.pickNetPort()
	if err == nil {
		err = srv.netJoin(port)
	} else {
		srv.net.mu.Lock()
		srv.net.lastErr = err.Error()
		srv.net.mu.Unlock()
	}
	if err != nil {
		log.Printf("LoRa network: %v", err)
	}
}

func main() {
	noTray := flag.Bool("no-tray", false, "run without a tray icon, logging to the console")
	allow := flag.String("allow-origin", "", "extra page origins or hosts to accept, comma separated")
	showVersion := flag.Bool("version", false, "print the version and exit")
	join := flag.Bool("join", false, "join the LoRa network at start (needs administrator rights)")
	takeover := flag.Bool("takeover", false, "replace a running instance (used by the elevated relaunch)")
	share := flag.Bool("share", false, "with -join: share this PC's internet with the LoRa network")
	useRoute := flag.Bool("use-internet-route", false, "with -use-internet: route ALL traffic through the gateway instead of setting the system proxy (background traffic will saturate the link)")
	useInet := flag.Bool("use-internet", false, "with -join: route this PC's internet through the network's gateway once one is heard")
	flag.Parse()
	if *showVersion {
		fmt.Println(appName, version)
		return
	}
	// Single-instance check before touching the log: a second copy must not rotate or
	// truncate the log file of the one already running.
	var ln net.Listener
	var port int
	var err error
	if *takeover {
		ln, port, err = listenTakeover(15 * time.Second)
	} else {
		ln, port, err = listen()
	}
	if errors.Is(err, errAlreadyRunning) {
		if *noTray {
			fmt.Fprintf(os.Stderr, "%s is already running on port %d\n", appName, port)
		}
		return
	}
	setupLog(*noTray)
	recoverStaleProxy()
	log.Printf("%s %s starting (%s/%s)", appName, version, runtime.GOOS, runtime.GOARCH)
	if err != nil {
		log.Fatalf("listen: %v", err)
	}
	log.Printf("listening on 127.0.0.1:%d", port)

	var extra []string
	for _, o := range strings.Split(*allow, ",") {
		if o = strings.TrimSpace(o); o != "" {
			extra = append(extra, o)
		}
	}

	srv := &server{version: version, extraOrigin: extra, status: func(string, bool) {}}
	srv.netUseRoute = *useRoute

	if *noTray {
		srv.status = func(text string, _ bool) { log.Printf("status: %s", text) }
		if *join {
			go joinNetworkWith(srv, *share, *useInet)
		}
		log.Fatal(srv.serve(ln))
		return
	}

	idleIcon := makeIcon(0x80, 0x88, 0x90)
	liveIcon := makeIcon(0x22, 0xA8, 0x55)

	systray.Run(func() {
		systray.SetIcon(idleIcon)
		systray.SetTitle("Voyager Link")
		tip := fmt.Sprintf("Voyager Link %s - port %d", version, port)
		systray.SetTooltip(tip + "\nIdle - waiting for the Voyager page")

		mStatus := systray.AddMenuItem("Idle - waiting for the Voyager page", "")
		mStatus.Disable()
		systray.AddSeparator()
		mAuto := systray.AddMenuItemCheckbox("Start with Windows", "Run Voyager Link when you sign in", autostartEnabled())
		if !autostartSupported() {
			mAuto.Hide()
		}
		systray.AddSeparator()
		mNet := systray.AddMenuItem("LoRa network: not joined", "")
		mNet.Disable()
		mJoin := systray.AddMenuItem("Join LoRa network", "Give this PC an address on the LoRa network (asks for administrator rights)")
		mLeave := systray.AddMenuItem("Leave LoRa network", "")
		mLeave.Hide()
		mShare := systray.AddMenuItemCheckbox("Share this PC's internet", "Be the LoRa network's internet gateway (very slow link: text-sized traffic only)", false)
		mShare.Hide()
		mUse := systray.AddMenuItemCheckbox("Use shared internet", "Route this PC's internet through the LoRa network's gateway", false)
		mUse.Hide()
		systray.AddSeparator()
		mLogs := systray.AddMenuItem("Open log folder", "")
		systray.AddSeparator()
		mQuit := systray.AddMenuItem("Quit", "Stop Voyager Link")

		srv.status = func(text string, connected bool) {
			log.Printf("status: %s", text)
			mStatus.SetTitle(text)
			systray.SetTooltip(tip + "\n" + text)
			if connected {
				systray.SetIcon(liveIcon)
			} else {
				systray.SetIcon(idleIcon)
			}
		}

		// Network menu state, refreshed every 2 s.
		go func() {
			for {
				st := srv.netStatus()
				switch {
				case st.Active:
					mNet.SetTitle(fmt.Sprintf("LoRa network: %s on %s (%d out, %d in, %d queued)", st.IP, st.Port, st.TX, st.RX, st.Queued))
					mJoin.Hide()
					mLeave.Show()
					mShare.Show()
					if st.Sharing {
						mShare.Check()
						mShare.SetTitle(fmt.Sprintf("Share this PC's internet (%d connections)", st.ShareFlows))
					} else {
						mShare.Uncheck()
						mShare.SetTitle("Share this PC's internet")
					}
					switch {
					case st.UsingGW || st.Gateway != "" && !st.Sharing:
						mUse.Show()
						if st.UsingGW {
							mUse.Check()
						} else {
							mUse.Uncheck()
						}
						title := "Use shared internet via " + st.Gateway + " (browser/apps via proxy, ~1 KB/s)"
						if srv.netUseRoute && st.OwnNet {
							// Honest: route mode takes over from the PC's own (fast) internet.
							title = "Use shared internet via " + st.Gateway + " (replaces this PC's internet, ~1 KB/s)"
						}
						mUse.SetTitle(title)
					default:
						mUse.Hide()
					}
				case st.Error != "":
					mNet.SetTitle("LoRa network: " + st.Error)
					mJoin.Show()
					mLeave.Hide()
				default:
					mNet.SetTitle("LoRa network: not joined")
					mJoin.Show()
					mLeave.Hide()
				}
				if !st.Active {
					mShare.Hide()
					mUse.Hide()
				}
				time.Sleep(2 * time.Second)
			}
		}()
		if *join {
			go joinNetworkWith(srv, *share, *useInet)
		}

		go func() {
			if err := srv.serve(ln); err != nil {
				log.Printf("server stopped: %v", err)
				systray.Quit()
			}
		}()

		go func() {
			for {
				select {
				case <-mAuto.ClickedCh:
					want := !mAuto.Checked()
					if err := setAutostart(want); err != nil {
						log.Printf("autostart: %v", err)
						continue
					}
					if want {
						mAuto.Check()
					} else {
						mAuto.Uncheck()
					}
				case <-mJoin.ClickedCh:
					if !isElevated() {
						// Creating the network adapter needs administrator rights: start an
						// elevated copy that takes over (same port, same tray), then leave.
						if err := relaunchElevated("-takeover -join"); err != nil {
							log.Printf("elevation: %v", err)
							mNet.SetTitle("LoRa network: administrator rights were not granted")
							continue
						}
						ln.Close()
						systray.Quit()
						return
					}
					go joinNetwork(srv)
				case <-mLeave.ClickedCh:
					go srv.netLeave("left")
				case <-mShare.ClickedCh:
					want := !mShare.Checked()
					go func() {
						if err := srv.netSetShare(want); err != nil {
							log.Printf("share internet: %v", err)
							mNet.SetTitle("LoRa network: " + err.Error())
						}
					}()
				case <-mUse.ClickedCh:
					want := !mUse.Checked()
					go func() {
						if err := srv.netSetUse(want); err != nil {
							log.Printf("use shared internet: %v", err)
							mNet.SetTitle("LoRa network: " + err.Error())
						}
					}()
				case <-mLogs.ClickedCh:
					_ = exec.Command("explorer", logDir()).Start()
				case <-mQuit.ClickedCh:
					srv.netLeave("left")
					systray.Quit()
					return
				}
			}
		}()
	}, func() {
		log.Printf("exiting")
	})
}
