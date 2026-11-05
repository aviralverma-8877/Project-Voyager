//go:build windows && amd64

package main

import _ "embed"

// Wintun 0.14.1 (WireGuard LLC, https://www.wintun.net), fetched and SHA-256 checked
// at build time by build.bat / the Pages workflow; never committed. Its license is
// wintun/LICENSE.txt. Written next to the exe before the adapter is created, because
// the loader only searches the application directory and System32.
//
//go:embed wintun/wintun.dll
var wintunDLL []byte
