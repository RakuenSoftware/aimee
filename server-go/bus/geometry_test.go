package bus

import "testing"

func TestInlineBudget(t *testing.T) {
	if (*Client)(nil).InlineBudget() != 0 {
		t.Fatal("nil client has an inline budget")
	}
	client := &Client{inlineBudget: 4096}
	if client.InlineBudget() != 4096 {
		t.Fatalf("budget = %d", client.InlineBudget())
	}
}

func TestNilClientHeartbeatNowIsSafe(t *testing.T) {
	(*Client)(nil).HeartbeatNow()
}
