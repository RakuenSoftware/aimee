package main

import (
	"sync"
	"time"
)

// rateLimiter is a tiny per-key failure limiter for the login path: after
// maxFailures within window, further attempts for that key are blocked until the
// window elapses. In-memory and single-instance (a documented constraint, same
// as the kb-side rate limits added in S2a).
type rateLimiter struct {
	mu          sync.Mutex
	failures    map[string]*failCount
	maxFailures int
	window      time.Duration
}

type failCount struct {
	n     int
	first time.Time
}

func newRateLimiter(max int, window time.Duration) *rateLimiter {
	return &rateLimiter{failures: map[string]*failCount{}, maxFailures: max, window: window}
}

func (rl *rateLimiter) allow(key string) bool {
	rl.mu.Lock()
	defer rl.mu.Unlock()
	fc := rl.failures[key]
	if fc == nil {
		return true
	}
	if time.Since(fc.first) > rl.window {
		delete(rl.failures, key)
		return true
	}
	return fc.n < rl.maxFailures
}

func (rl *rateLimiter) fail(key string) {
	rl.mu.Lock()
	defer rl.mu.Unlock()
	fc := rl.failures[key]
	if fc == nil || time.Since(fc.first) > rl.window {
		rl.failures[key] = &failCount{n: 1, first: time.Now()}
		return
	}
	fc.n++
}

func (rl *rateLimiter) reset(key string) {
	rl.mu.Lock()
	defer rl.mu.Unlock()
	delete(rl.failures, key)
}
