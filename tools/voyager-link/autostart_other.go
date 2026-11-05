//go:build !windows

package main

import "errors"

func autostartSupported() bool   { return false }
func autostartEnabled() bool     { return false }
func setAutostart(on bool) error { return errors.New("start at login is only supported on Windows") }
