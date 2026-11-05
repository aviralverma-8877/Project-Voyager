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

## LoRa network (TCP/IP over LoRa)

Every PC running Voyager Link with its board plugged in can join one shared IP
network over LoRa: tray menu, **Join LoRa network**.

- Each board has a fixed address from its MAC, `10.73.<x>.<y>`, which the PC
  takes on a virtual adapter called `Voyager-LoRa` (Wintun). No configuration is needed.
- Every board within radio range of another can reach it: ping, ssh, HTTP, chat over
  TCP. Speed is roughly 0.5–1 KB/s **shared** by all nodes at the default radio
  settings, with about 0.5 s round trips. Fine for text and small files, not for browsing.
- Creating the adapter needs **administrator rights**: Join restarts Voyager Link
  through a UAC prompt. Leave or Quit removes the adapter, and the board goes back to
  normal within 30 s even if the PC just disappears.
- Only unicast IPv4 inside `10.73.0.0/16` is sent over the radio. Windows' broadcast
  and multicast chatter and IPv6 are dropped. TCP connections are clamped to
  190-byte segments so every packet fits one LoRa frame. Other packets over 247 bytes are dropped.
- A firewall rule lets peers **ping** this PC. Any other service you want to offer
  (ssh, a web server) needs its own inbound rule, on purpose.
- While joined, the board's COM port belongs to the network: the device's File
  Transfer page can't use it until you leave.

### Internet sharing

One PC that has internet can share it with everyone on the LoRa network.

- **Gateway:** tray menu, **Share this PC's internet**. Voyager Link runs a
  user-space NAT, an in-process TCP/IP stack (gVisor). It accepts the TCP/UDP
  connections that peers send to the internet and opens them again from this PC.
  Windows networking (ICS, routes) isn't touched. Peers can only reach **public**
  internet addresses, never this PC or its LAN (private, loopback, link-local and
  CGNAT ranges are refused). The gateway announces itself on the network every 30 s.
- **Client:** once a gateway has been heard, the tray offers **Use shared internet via
  10.73.x.y**. This adds a default route and DNS (1.1.1.1, 8.8.8.8) on the
  `Voyager-LoRa` adapter. Untick it, or leave the network, to remove both.
- **Speed:** it's the same 0.5–1 KB/s link, shared by everyone. It's meant for
  text-sized traffic: messaging, email text, API calls, `curl`, small pages. A 2 MB
  web page takes about an hour. UDP datagrams over 247 bytes from a client (for
  example QUIC) are dropped, so browsers fall back to TCP.
- Headless: `-join -share` makes a gateway; `-join -use-internet` waits for a gateway
  and uses it.
- Needs board firmware with network protocol 2. If the board reboots while joined,
  Voyager Link notices within 20 s (no keepalive answer) and rejoins on its own.

Wintun is © WireGuard LLC (https://www.wintun.net), distributed under its prebuilt
binaries license (`wintun/LICENSE.txt`, fetched with the driver at build time).

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

The first build fetches Wintun 0.14.1 (`fetch-wintun.ps1`, SHA-256 pinned) into `wintun/`
(git-ignored), which gets embedded into the exe. It then runs vet and the tests, and writes `dist\voyager-link.exe` (about 8.5 MB, a single static
binary with no console window). Attach it to the GitHub release as `voyager-link.exe`.
The page links to `releases/latest/download/voyager-link.exe`. Don't compress it
with UPX, because antivirus tools flag UPX-packed binaries.

Command-line flags: `-no-tray` (console logging, for debugging), `-allow-origin`, `-version`,
`-join` (join the LoRa network at start; needs administrator rights), `-share`, `-use-internet`.
