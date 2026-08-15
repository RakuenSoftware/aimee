package economizer

import (
	"sync"
	"time"
)

// The gateway's per-session circuit breaker.
//
// This is the economizer's OWN state: "I have switched my reduction lever off
// for this session, until this moment." It is not the caller's session state and
// carries nothing about the caller beyond the opaque key it was handed — no
// history, no correlation between turns, no idempotency on anyone's behalf. The
// module may hold it precisely because it is the module's own business.
//
// Volatile by design, exactly as the C table it replaces was. A breaker that
// forgets on restart fails OPEN: reduction resumes and, if the payload is still
// bad, the next rejection trips it again within one turn. Persisting it would
// mean a restart could not clear a stuck breaker, which is the worse failure.

const (
	// breakerCap bounds the table. The C used 10000 with a 16384-slot open
	// addressing table; a Go map needs no load factor, so only the cap carries
	// over. Reaching it means thousands of sessions are concurrently disabled —
	// the catastrophic incident the breaker exists for — and the bound is what
	// keeps that from also becoming a memory problem.
	breakerCap = 10000
	// breakerSweepAbove is the fill level past which an insert sweeps expired
	// entries first, so a LIVE disable is never evicted while a dead one remains.
	breakerSweepAbove = breakerCap / 2
)

type breakerEntry struct {
	expires time.Time
	seq     uint64
	reason  string
}

// SessionBreaker is safe for concurrent use.
type SessionBreaker struct {
	mu      sync.Mutex
	entries map[string]breakerEntry
	seq     uint64
}

func NewSessionBreaker() *SessionBreaker {
	return &SessionBreaker{entries: make(map[string]breakerEntry)}
}

// IsDisabled reports whether the lever is currently off for key.
//
// Expiry is lazy: an entry found past its deadline is dropped here rather than
// waiting for a sweep, so the common lookup path also does the cleanup.
func (b *SessionBreaker) IsDisabled(key string) bool {
	if b == nil || key == "" {
		return false
	}
	now := time.Now()
	b.mu.Lock()
	defer b.mu.Unlock()
	e, ok := b.entries[key]
	if !ok {
		return false
	}
	if e.expires.After(now) {
		return true
	}
	delete(b.entries, key)
	return false
}

// Disable trips the breaker for ttlMS milliseconds.
//
// A non-positive TTL is rejected rather than treated as "forever": it would
// otherwise mean a misconfigured zero silently disables the lever permanently.
// Re-disabling a live session refreshes both the window and the reason, so the
// breaker reports why it is CURRENTLY off, not why it first tripped.
func (b *SessionBreaker) Disable(key string, ttlMS int, reason string) {
	if b == nil || key == "" || ttlMS <= 0 {
		return
	}
	if reason == "" {
		reason = "unknown"
	}
	now := time.Now()
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.entries == nil {
		b.entries = make(map[string]breakerEntry)
	}
	if e, ok := b.entries[key]; ok {
		e.expires = now.Add(time.Duration(ttlMS) * time.Millisecond)
		e.reason = reason
		b.entries[key] = e
		return
	}
	if len(b.entries) > breakerSweepAbove {
		b.sweepLocked(now)
	}
	if len(b.entries) >= breakerCap {
		b.evictOldestLocked()
	}
	b.seq++
	b.entries[key] = breakerEntry{
		expires: now.Add(time.Duration(ttlMS) * time.Millisecond),
		seq:     b.seq,
		reason:  reason,
	}
}

// Reason returns why the lever is off for key, empty when it is not.
func (b *SessionBreaker) Reason(key string) string {
	if b == nil || key == "" {
		return ""
	}
	now := time.Now()
	b.mu.Lock()
	defer b.mu.Unlock()
	if e, ok := b.entries[key]; ok && e.expires.After(now) {
		return e.reason
	}
	return ""
}

func (b *SessionBreaker) sweepLocked(now time.Time) {
	for k, e := range b.entries {
		if !e.expires.After(now) {
			delete(b.entries, k)
		}
	}
}

// evictOldestLocked drops the entry disabled longest ago. Callers sweep first,
// so this only ever runs when the table is full of LIVE disables and something
// has to go; oldest-first means the survivor set is the most recent trouble.
func (b *SessionBreaker) evictOldestLocked() {
	var oldestKey string
	var oldestSeq uint64
	first := true
	for k, e := range b.entries {
		if first || e.seq < oldestSeq {
			oldestKey, oldestSeq, first = k, e.seq, false
		}
	}
	if !first {
		delete(b.entries, oldestKey)
	}
}
