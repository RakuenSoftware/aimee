package bus

import (
	"encoding/binary"
	"sync/atomic"
	"testing"
	"unsafe"
)

// The region attach paths treat their headers as written by another process, so
// a corrupt header must be refused rather than propagated into a mapping size or
// a ring walk. These build valid headers in Go, then poke each field.
func TestControlAttachRejectsCorruption(t *testing.T) {
	mem := make([]byte, ctlBytes)
	binary.LittleEndian.PutUint32(mem[ctlOffSpec:], specVersion)
	binary.LittleEndian.PutUint32(mem[ctlOffLayout:], layoutVersion)
	binary.LittleEndian.PutUint32(mem[ctlOffSlot:], 256)
	binary.LittleEndian.PutUint32(mem[ctlOffInline:], 192)
	binary.LittleEndian.PutUint32(mem[ctlOffQueueCap:], 16)
	binary.LittleEndian.PutUint64(mem[ctlOffArena:], 4096)
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&mem[ctlOffMagic])), ctlMagic)

	if _, err := AttachControl(mem); err != nil {
		t.Fatalf("valid control refused: %v", err)
	}

	cases := []struct {
		name string
		poke func()
	}{
		{"bad magic", func() { binary.LittleEndian.PutUint32(mem[ctlOffMagic:], 0) }},
		{"bad spec", func() { binary.LittleEndian.PutUint32(mem[ctlOffSpec:], 99) }},
		{"bad layout", func() { binary.LittleEndian.PutUint32(mem[ctlOffLayout:], 99) }},
		{"zero slot", func() { binary.LittleEndian.PutUint32(mem[ctlOffSlot:], 0) }},
		{"inline past slot", func() { binary.LittleEndian.PutUint32(mem[ctlOffInline:], 999) }},
		{"zero arena", func() { binary.LittleEndian.PutUint64(mem[ctlOffArena:], 0) }},
		{"short", nil},
	}
	for _, tc := range cases {
		saved := make([]byte, ctlBytes)
		copy(saved, mem)
		var target []byte = mem
		if tc.poke != nil {
			tc.poke()
		} else {
			target = mem[:ctlBytes-1]
		}
		if _, err := AttachControl(target); err == nil {
			t.Fatalf("control attach accepted corruption: %s", tc.name)
		}
		copy(mem, saved)
	}
}
