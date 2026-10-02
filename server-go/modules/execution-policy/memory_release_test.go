package executionpolicy

import (
	"strings"
	"testing"
	"time"
)

func TestMemoryReleaseIsExplicitRecipientAndResourceBound(t *testing.T) {
	now := time.Now()
	p := MemoryReleasePolicy{Version: 1, Recipient: "model-a", PeerUID: 42, Namespace: "user-a", BaseView: strings.Repeat("a", 64), Active: true, ValidUntil: now.Add(time.Hour).Format(time.RFC3339Nano), Records: []MemoryReleaseRule{{ID: "5", ScopeType: "user", ScopeValue: "_user", FollowUpdates: true}, {ID: "6", ScopeType: "user", ScopeValue: "_user", Revision: "old"}}}
	if e := p.Validate("model-a", 42, now); e != nil {
		t.Fatal(e)
	}
	for _, x := range []struct {
		recipient string
		uid       uint32
		now       time.Time
	}{{"model-b", 42, now}, {"model-a", 43, now}, {"model-a", 42, now.Add(2 * time.Hour)}} {
		if p.Validate(x.recipient, x.uid, x.now) == nil {
			t.Fatal("wrong principal or expired release allowed")
		}
	}
	if !p.Allows("user-a", "5", "new", "user", "_user") || p.Allows("user-a", "6", "new", "user", "_user") || p.Allows("user-a", "999", "new", "user", "_user") || p.Allows("user-b", "5", "new", "user", "_user") || p.Allows("user-a", "5", "new", "project", "private") {
		t.Fatal("release widened")
	}
	p.Active = false
	if p.Validate("model-a", 42, now) == nil {
		t.Fatal("withdrawal allowed")
	}
}
