package main

import (
	"bytes"
	"encoding/binary"
)

// makeIcon builds a 32x32 .ico (32-bit BMP payload) at runtime: a filled circle in
// the given colour with a white "signal" dot, so no binary assets live in the repo.
// Tray state: grey = idle, green = page connected.
func makeIcon(r, g, b byte) []byte {
	const n = 32
	pixels := make([]byte, 0, n*n*4)
	// BMP rows are bottom-up.
	for y := n - 1; y >= 0; y-- {
		for x := 0; x < n; x++ {
			dx, dy := float64(x)-15.5, float64(y)-15.5
			d2 := dx*dx + dy*dy
			var a, cr, cg, cb byte
			switch {
			case d2 <= 4.5*4.5:
				a, cr, cg, cb = 255, 255, 255, 255
			case d2 <= 15*15:
				a, cr, cg, cb = 255, r, g, b
			case d2 <= 16*16:
				a, cr, cg, cb = 128, r, g, b // soft edge
			}
			pixels = append(pixels, cb, cg, cr, a) // BGRA
		}
	}
	mask := make([]byte, n*(n/8)) // AND mask, all 0: alpha channel decides

	var bmp bytes.Buffer
	binary.Write(&bmp, binary.LittleEndian, struct {
		Size                            uint32
		Width, Height                   int32
		Planes, BitCount                uint16
		Compression, ImageSize          uint32
		XPels, YPels, ClrUsed, ClrImpor int32
	}{40, n, n * 2, 1, 32, 0, uint32(len(pixels) + len(mask)), 0, 0, 0, 0})
	bmp.Write(pixels)
	bmp.Write(mask)

	var ico bytes.Buffer
	binary.Write(&ico, binary.LittleEndian, []uint16{0, 1, 1}) // reserved, type=icon, count
	binary.Write(&ico, binary.LittleEndian, struct {
		W, H, Colors, Reserved byte
		Planes, BitCount       uint16
		Size, Offset           uint32
	}{n, n, 0, 0, 1, 32, uint32(bmp.Len()), 6 + 16})
	ico.Write(bmp.Bytes())
	return ico.Bytes()
}
