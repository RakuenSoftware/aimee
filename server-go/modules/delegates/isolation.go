package delegates

import (
	"encoding/json"
	"strings"
)

// Whether a container that was asked for no network actually got none, and what
// to do when that cannot be established.
//
// `--network none` is a request to the runtime, not a guarantee from it. If the
// runtime does not honour it, the delegate reaches the network directly and
// bypasses the egress proxy and its allowlist entirely -- the isolation the
// whole design rests on is simply absent, silently. So the container is probed
// after it starts, and this decides what the probe means.
//
// The probe itself is I/O and belongs to the caller. What its result IMPLIES is
// a decision and lives here, where it can be tested without a container.

// IsolationProbe is what the caller observed about the container's networks.
type IsolationProbe int

const (
	// IsolationUnknown means the probe could not answer -- it failed, timed
	// out, or returned something unparseable. NOT the same as isolated.
	IsolationUnknown IsolationProbe = iota
	// IsolationConfirmed means no network is attached and none has an address.
	IsolationConfirmed
	// IsolationBreached means the container can reach the network.
	IsolationBreached
)

// IsolationVerdict is what to do with the run.
type IsolationVerdict struct {
	// Refuse means do not run this delegate, and destroy the container.
	Refuse bool
	// Reason is operator-facing and states what was observed and why it
	// matters, never just "failed".
	Reason string
}

// JudgeIsolation decides whether a delegate may run.
//
// A breach and an unknown result both refuse unconditionally. An enabled
// sandbox is never advisory: "the probe failed" and "the sandbox is open" are
// indistinguishable at this boundary.
func JudgeIsolation(probe IsolationProbe) IsolationVerdict {
	switch probe {
	case IsolationBreached:
		reason := "container has network egress despite being created with no network: " +
			"the runtime did not honour isolation, so the delegate can reach the network " +
			"directly and bypass the egress proxy and its allowlist"
		return IsolationVerdict{Refuse: true, Reason: reason + " -- refusing to run"}

	case IsolationUnknown:
		return IsolationVerdict{Refuse: true,
			Reason: "could not verify network isolation -- refusing to run a delegate that cannot be proven isolated"}
	}
	return IsolationVerdict{}
}

// ParseIsolationProbe reads a container runtime's network report.
//
// The expected shape is "<network>=<ip>;" repeated, which is what docker's
// inspect template produces. A container is isolated only when every entry is
// the "none" network AND carries no address: a named network means attachment,
// and an address means reachability, so either alone is a breach.
//
// UNPARSEABLE input is UNKNOWN, never isolated: a line that does not match the
// contract is not evidence of safety. An EMPTY report is different and IS
// isolated -- docker prints nothing when the network map is empty, so silence
// here is the answer rather than the absence of one. A probe that could not run
// at all arrives as probeFailed, which is what covers the silence that means
// nothing was measured.
func ParseIsolationProbe(report string, probeFailed bool) IsolationProbe {
	if probeFailed {
		return IsolationUnknown
	}
	trimmed := strings.TrimSpace(report)
	if trimmed == "" {
		// No networks at all is a legitimate isolated result: docker prints
		// nothing when the map is empty.
		return IsolationConfirmed
	}
	if strings.HasPrefix(trimmed, "{") {
		var networks map[string]struct {
			IPAddress         string `json:"IPAddress"`
			GlobalIPv6Address string `json:"GlobalIPv6Address"`
		}
		if err := json.Unmarshal([]byte(trimmed), &networks); err != nil {
			return IsolationUnknown
		}
		for name, network := range networks {
			if name != "none" || network.IPAddress != "" || network.GlobalIPv6Address != "" {
				return IsolationBreached
			}
		}
		return IsolationConfirmed
	}

	sawEntry := false
	for _, entry := range strings.FieldsFunc(trimmed, func(r rune) bool {
		return r == ';' || r == '\n'
	}) {
		entry = strings.TrimSpace(entry)
		if entry == "" {
			continue
		}
		name, ip, found := strings.Cut(entry, "=")
		if !found {
			// A line that does not match the contract is not evidence of
			// safety.
			return IsolationUnknown
		}
		sawEntry = true
		name = strings.TrimSpace(name)
		ip = strings.TrimSpace(ip)
		if ip != "" || (name != "" && name != "none") {
			return IsolationBreached
		}
	}
	if !sawEntry {
		return IsolationUnknown
	}
	return IsolationConfirmed
}
