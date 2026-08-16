package bus

// InlineBudget is the maximum payload carried by one inline event on this
// attachment. Protocol adapters use it to fragment request/reply streams and
// to apply their own notification chunk envelopes without guessing host
// geometry.
func (c *Client) InlineBudget() uint32 {
	if c == nil {
		return 0
	}
	return c.inlineBudget
}

// HeartbeatNow advances liveness with the same monotonic clock used by the
// module runtime and host stale-client checks.
func (c *Client) HeartbeatNow() {
	if c == nil {
		return
	}
	if now := monotonicNowNS(); now != 0 {
		c.Heartbeat(now)
	}
}
