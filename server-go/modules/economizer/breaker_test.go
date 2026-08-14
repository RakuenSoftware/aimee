package economizer

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestBreakerTripsAndExpires(t *testing.T) {
	b := NewSessionBreaker()
	if b.IsDisabled("k") {
		t.Fatal("a fresh breaker disables nothing")
	}
	b.Disable("k", 60_000, "4xx")
	if !b.IsDisabled("k") {
		t.Error("a tripped session should read as disabled")
	}
	if b.Reason("k") != "4xx" {
		t.Errorf("reason = %q, want 4xx", b.Reason("k"))
	}
	if b.IsDisabled("other") {
		t.Error("tripping one session must not disable another")
	}

	// An elapsed window reads as enabled again, and the entry is dropped.
	b.Disable("short", 1, "5xx")
	time.Sleep(5 * time.Millisecond)
	if b.IsDisabled("short") {
		t.Error("an expired disable should have lapsed")
	}
}

func TestBreakerRejectsWhatCannotBeATrip(t *testing.T) {
	b := NewSessionBreaker()
	// A non-positive TTL would otherwise mean "disabled forever" — a
	// misconfigured zero must not silently switch the lever off permanently.
	b.Disable("k", 0, "4xx")
	b.Disable("k", -1, "4xx")
	if b.IsDisabled("k") {
		t.Error("a non-positive TTL must not disable")
	}
	// No key means no lever; disabling must not become a global switch.
	b.Disable("", 60_000, "4xx")
	if b.IsDisabled("") {
		t.Error("the empty key must never read as disabled")
	}
}

func TestBreakerRedisableRefreshesTheReason(t *testing.T) {
	b := NewSessionBreaker()
	b.Disable("k", 60_000, "4xx")
	b.Disable("k", 60_000, "5xx")
	// The breaker reports why it is CURRENTLY off, not why it first tripped.
	if got := b.Reason("k"); got != "5xx" {
		t.Errorf("reason = %q, want 5xx", got)
	}
}

func TestBreakerStaysBounded(t *testing.T) {
	b := NewSessionBreaker()
	for i := 0; i < breakerCap+500; i++ {
		b.Disable(string(rune('a'+i%26))+string(rune(i)), 60_000, "4xx")
	}
	b.mu.Lock()
	n := len(b.entries)
	b.mu.Unlock()
	if n > breakerCap {
		t.Errorf("breaker holds %d entries, cap is %d", n, breakerCap)
	}
}

func postStatus(t *testing.T, h bus.ModuleHandler, req PostStatusRequest) PostStatusResponse {
	t.Helper()
	body, err := json.Marshal(req)
	if err != nil {
		t.Fatal(err)
	}
	out, st := h(bus.ModuleInvocation{StageID: StagePostStatus}, body)
	if st != bus.ModuleStatusOK {
		t.Fatalf("status = %v", st)
	}
	var resp PostStatusResponse
	if err := json.Unmarshal(out, &resp); err != nil {
		t.Fatal(err)
	}
	return resp
}

func TestPostStatusStageDecides(t *testing.T) {
	h := NewHandler()

	// 4xx: our payload was rejected, so restore, trip and resend once.
	got := postStatus(t, h, PostStatusRequest{
		SessionKey: "s1", HTTPStatus: 413, Mutated: true, TTLMS: 60_000,
	})
	if got.Action != "resend" || !got.Restore || !got.Disabled {
		t.Errorf("4xx = %+v, want resend+restore+disabled", got)
	}
	if got.Counter != Stat4xxRestoreResend {
		t.Errorf("counter = %q", got.Counter)
	}

	// 5xx: provider state is uncertain, so trip but never resend.
	got = postStatus(t, h, PostStatusRequest{
		SessionKey: "s2", HTTPStatus: 503, Mutated: true, TTLMS: 60_000,
	})
	if got.Action != "none" || got.Restore || !got.Disabled {
		t.Errorf("5xx = %+v, want none+no-restore+disabled", got)
	}

	// 2xx owes nothing, and neither does a turn that was never mutated: an
	// unrelated provider error must not switch the lever off.
	for _, tc := range []PostStatusRequest{
		{SessionKey: "s3", HTTPStatus: 200, Mutated: true, TTLMS: 60_000},
		{SessionKey: "s4", HTTPStatus: 400, Mutated: false, TTLMS: 60_000},
	} {
		got = postStatus(t, h, tc)
		if got.Action != "none" || got.Disabled {
			t.Errorf("%+v -> %+v, want inert", tc, got)
		}
	}
}

func TestPostStatusStageStreamPath(t *testing.T) {
	h := NewHandler()
	// Streaming: bytes are with the client, so trip but never resend.
	got := postStatus(t, h, PostStatusRequest{
		SessionKey: "s1", Mutated: true, HaveKey: true,
		TTLMS: 60_000, StreamReason: "stream_invalid_request",
	})
	if got.Action != "none" || got.Restore {
		t.Errorf("stream must not resend or restore: %+v", got)
	}
	if !got.Disabled || got.Reason != "stream_invalid_request" {
		t.Errorf("stream = %+v, want disabled with its reason", got)
	}

	// Without a key there is nothing to disable, so nothing is claimed.
	got = postStatus(t, h, PostStatusRequest{
		Mutated: true, HaveKey: false, TTLMS: 60_000, StreamReason: "stream",
	})
	if got.Disabled {
		t.Error("no session key must not report a trip")
	}
}

// A trip is only worth anything if the NEXT turn sees it. This is the round trip
// the C seam never had: its breaker was written on a path nothing called, so the
// pre-send read was permanently false. Both halves live here now, so one handler
// can prove the loop closes.
func TestTrippedBreakerBlocksTheNextReduction(t *testing.T) {
	h := NewHandler()

	reduce := func(key string) ReduceResponse {
		t.Helper()
		body, err := json.Marshal(ReduceRequest{
			Messages:      rawMessages(t, 20),
			SystemPrompt:  "sys",
			Seam:          "gateway",
			SessionKey:    key,
			HistoryFold:   true,
			ClosetEnabled: true,
		})
		if err != nil {
			t.Fatal(err)
		}
		out, st := h(bus.ModuleInvocation{StageID: StageReduce}, body)
		if st != bus.ModuleStatusOK {
			t.Fatalf("status = %v", st)
		}
		var resp ReduceResponse
		if err := json.Unmarshal(out, &resp); err != nil {
			t.Fatal(err)
		}
		return resp
	}

	if before := reduce("sess"); !before.Mutated {
		t.Fatalf("expected a reduction before the breaker trips: %+v", before)
	}

	postStatus(t, h, PostStatusRequest{
		SessionKey: "sess", HTTPStatus: 413, Mutated: true, TTLMS: 60_000,
	})

	after := reduce("sess")
	if after.Mutated {
		t.Error("a disabled session must pass through pristine")
	}
	if after.Bypass != "session_disabled" {
		t.Errorf("bypass = %q, want session_disabled", after.Bypass)
	}

	// The breaker is per session, so an untouched identity still reduces.
	if other := reduce("elsewhere"); !other.Mutated {
		t.Error("one session's breaker must not disable another's")
	}
}
