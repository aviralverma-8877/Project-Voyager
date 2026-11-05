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
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

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

func main() {
	noTray := flag.Bool("no-tray", false, "run without a tray icon, logging to the console")
	allow := flag.String("allow-origin", "", "extra page origins or hosts to accept, comma separated")
	showVersion := flag.Bool("version", false, "print the version and exit")
	flag.Parse()
	if *showVersion {
		fmt.Println(appName, version)
		return
	}
	// Single-instance check before touching the log: a second copy must not rotate or
	// truncate the log file of the one already running.
	ln, port, err := listen()
	if errors.Is(err, errAlreadyRunning) {
		if *noTray {
			fmt.Fprintf(os.Stderr, "%s is already running on port %d\n", appName, port)
		}
		return
	}
	setupLog(*noTray)
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

	if *noTray {
		srv.status = func(text string, _ bool) { log.Printf("status: %s", text) }
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
				case <-mLogs.ClickedCh:
					_ = exec.Command("explorer", logDir()).Start()
				case <-mQuit.ClickedCh:
					systray.Quit()
					return
				}
			}
		}()
	}, func() {
		log.Printf("exiting")
	})
}
