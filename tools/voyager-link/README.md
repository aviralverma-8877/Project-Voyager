# Voyager Link

A small Windows tray helper that lets the Voyager web page use the device's USB
serial port for **File Transfer** (Kermit).

Chrome only allows Web Serial on secure pages (HTTPS or `localhost`). The device
serves its UI over plain `http://`, so the page connects to Voyager Link on
`ws://127.0.0.1:47321` instead, and Voyager Link talks to the COM port.

```
Voyager page (http://192.168.4.1) ──ws://127.0.0.1:47321/serial──> voyager-link.exe ──COM port──> ESP32
```

## Using it

1. Download `voyager-link.exe` from the latest GitHub release while you are online.
2. Run it. There is no installer and no admin rights are needed. A grey dot shows up
   in the system tray. It turns green while a Voyager page is connected.
3. If SmartScreen warns about an unknown publisher: **More info → Run anyway**.
4. Open **File Transfer** on the device page. If Chrome asks to let the page access
   apps and services on this device, choose **Allow**.
5. Tray menu → **Start with Windows** keeps it running across reboots
   (`HKCU\...\Run`, per user).

Close any serial monitor (Arduino IDE, PlatformIO, PuTTY) first, because only one program can
open a COM port at a time.

Logs: `%LOCALAPPDATA%\VoyagerLink\voyager-link.log` (tray → **Open log folder**).

## Security

- Listens on loopback only (`127.0.0.1`), never on the network.
- Accepts pages served from private, link-local or loopback IPs, `.local` or
  `localhost` only. Any other website open in the browser is refused (403), because
  WebSockets aren't protected by CORS. Add more origins with
  `-allow-origin host1,host2`.
- Several pages can be connected at once (up to 8), for example one tab per board when
  both boards are plugged into the same PC. Each COM port belongs to one page. If
  another page tries to open a port that's already in use, it gets an error naming
  the page that holds it.
- DTR/RTS are held low when opening the port, so opening it doesn't reset the
  ESP32.

## Protocol (`/serial`)

| Direction | Frame | Content |
|---|---|---|
| page → helper | text | `{"op":"list"}` · `{"op":"open","port":"COM5","baud":115200}` · `{"op":"close"}` |
| helper → page | text | `{"op":"hello"}` · `{"op":"ports","ports":[{name,vid,pid,desc,likely,usb}]}` · `{"op":"opened"}` · `{"op":"closed","reason"}` · `{"op":"error","msg"}` |
| both ways | binary | raw serial bytes |

`GET /info` returns `{"app":"voyager-link","version":"…","busy":bool}`. The page probes
ports 47321–47325 (`helperPorts` in `server.go`, `HELPER_PORTS` in
`data_main/script.js`; keep them in sync).

## Building

Requires Go 1.22 or later.

```bat
tools\voyager-link\build.bat 1.0.0
```

This runs vet and the tests, then writes `dist\voyager-link.exe` (about 8 MB, a single static
binary with no console window). Attach it to the GitHub release as `voyager-link.exe`.
The page links to `releases/latest/download/voyager-link.exe`. Don't compress it
with UPX, because antivirus tools flag UPX-packed binaries.

Command-line flags: `-no-tray` (console logging, for debugging), `-allow-origin`, `-version`.
