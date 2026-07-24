package bus

import "testing"

// The Go ring must agree with the C layout (offsets frozen by the C static
// asserts). This builds a ring in Go and round-trips it, which checks the Go
// implementation is self-consistent at the documented offsets; the full
// cross-language interop against the live C host is the conformance slice.
func TestRingRoundTrip(t *testing.T) {
	const slot, cap = 64, 8
	mem := make([]byte, ringHdrBytes+slot*cap)
	r, err := initRing(mem, slot, cap)
	if err != nil {
		t.Fatalf("initRing: %v", err)
	}
	if r.Capacity() != cap || r.Count() != 0 {
		t.Fatalf("fresh ring: cap %d count %d", r.Capacity(), r.Count())
	}

	// Fill to capacity, then it must refuse.
	for i := 0; i < cap; i++ {
		s := r.ProduceBegin()
		if s == nil {
			t.Fatalf("produce %d unexpectedly full", i)
		}
		s[0] = byte(i)
		r.ProduceCommit()
	}
	if r.ProduceBegin() != nil {
		t.Fatal("full ring accepted a producer")
	}
	if r.Count() != cap {
		t.Fatalf("count at capacity: %d", r.Count())
	}

	// Drain in FIFO order.
	for i := 0; i < cap; i++ {
		s := r.ConsumeBegin()
		if s == nil || s[0] != byte(i) {
			t.Fatalf("consume %d: %v", i, s)
		}
		r.ConsumeCommit()
	}
	if r.ConsumeBegin() != nil {
		t.Fatal("drained ring returned a slot")
	}

	// Many laps, so slots are reused and a wrapping index would alias.
	for lap := 0; lap < 10000; lap++ {
		s := r.ProduceBegin()
		if s == nil {
			t.Fatalf("wrap produce %d", lap)
		}
		s[0] = byte(lap)
		s[1] = byte(lap >> 8)
		r.ProduceCommit()
		c := r.ConsumeBegin()
		if c == nil || c[0] != byte(lap) || c[1] != byte(lap>>8) {
			t.Fatalf("wrap consume %d", lap)
		}
		r.ConsumeCommit()
	}
	if r.Count() != 0 {
		t.Fatalf("count after wrap: %d", r.Count())
	}

	// A truncated buffer is refused.
	if _, err := AttachRing(mem[:ringHdrBytes-1]); err == nil {
		t.Fatal("attach accepted a short buffer")
	}
}
