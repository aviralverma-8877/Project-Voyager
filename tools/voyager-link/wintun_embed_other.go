//go:build windows && !amd64

package main

// Only the amd64 build ships the Wintun driver.
var wintunDLL []byte
