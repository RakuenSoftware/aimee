package economizer

import (
	"fmt"
	"strings"
	"testing"
)

// --- test doubles ---

type fakeSessions struct {
	key      string
	resolve  bool
	disabled bool
	disables []string // "key|ttl|reason"
}

func (f *fakeSessions) ResolveKey(_, _, _ string) (string, bool) { return f.key, f.resolve }
func (f *fakeSessions) IsDisabled(string) bool                   { return f.disabled }
func (f *fakeSessions) Disable(key string, ttlMS int, reason string) {
	f.disables = append(f.disables, fmt.Sprintf("%s|%d|%s", key, ttlMS, reason))
}

type fakeStats struct {
	counts  map[string]int
	reasons []string
	deltas  [][2]int
}

func newStats() *fakeStats        { return &fakeStats{counts: map[string]int{}} }
func (s *fakeStats) Inc(c string) { s.counts[c]++ }
func (s *fakeStats) IncReason(c, reason string) {
	s.counts[c]++
	s.reasons = append(s.reasons, c+":"+reason)
}
func (s *fakeStats) RecordTokenDelta(b, r int) { s.deltas = append(s.deltas, [2]int{b, r}) }

func wireDeps(sess *fakeSessions, st *fakeStats) GatewayDeps {
	return GatewayDeps{Sessions: sess, Stats: st, Repair: cleanRepair}
}

func onCfg() GatewayConfig {
	return GatewayConfig{MutateOn: true, SessionDisableTTLMS: 3600000, FoldRetainedMsgs: 4}
}

// A container whose message array is fat enough that compression genuinely
// shrinks it.
func wireContainer() *JSONValue {
	c := NewObject()
	msgs := NewArray()
	msgs.Append(mkUser("do the task"))
	for k := 0; k < 6; k++ {
		id := fmt.Sprintf("call_%02d", k)
		a := NewObject()
		a.Set("role", NewString("assistant"))
		tcs := NewArray()
		tc := NewObject()
		tc.Set("id", NewString(id))
		tc.Set("type", NewString("function"))
		fn := NewObject()
		fn.Set("name", NewString("read_file"))
		fn.Set("arguments", NewString("{}"))
		tc.Set("function", fn)
		tcs.Append(tc)
		a.Set("tool_calls", tcs)
		msgs.Append(a)

		var body strings.Builder
		for body.Len() < 800 {
			body.WriteString("filler output bytes here and on; ")
		}
		fmt.Fprintf(&body, "tail at /work/src/stage_%d.c done", k)
		tr := NewObject()
		tr.Set("role", NewString("tool"))
		tr.Set("tool_call_id", NewString(id))
		tr.Set("content", NewString(body.String()))
		msgs.Append(tr)
	}
	c.Set("messages", msgs)
	return c
}

func ident() RequestIdentity {
	return RequestIdentity{SessionHeader: "s1", Bearer: "b", AuthIdentity: "a"}
}

// --- tests ---

func TestWireMutateAppliesAndRecords(t *testing.T) {
	c := wireContainer()
	before := PrintJSONUnformatted(c.Get("messages"))
	sess := &fakeSessions{key: "k1", resolve: true}
	st := newStats()
	var ctx MutateCtx

	BufferedMutate(c, "messages", "gpt-4o", "sys", ident(), onCfg(), wireDeps(sess, st), &ctx)

	if !ctx.Mutated {
		t.Fatalf("expected a mutation; stats=%v reasons=%v", st.counts, st.reasons)
	}
	if st.counts[StatMutateAttempted] != 1 || st.counts[StatMutateApplied] != 1 {
		t.Errorf("counters wrong: %v", st.counts)
	}
	if len(st.deltas) != 1 || st.deltas[0][1] >= st.deltas[0][0] {
		t.Errorf("token delta should show a shrink: %v", st.deltas)
	}
	if PrintJSONUnformatted(c.Get("messages")) == before {
		t.Error("the container was not actually mutated")
	}
	// The pristine copy must survive for a possible restore.
	if ctx.Pristine == nil || PrintJSONUnformatted(ctx.Pristine) != before {
		t.Error("the pristine snapshot was lost or altered")
	}
}

// The gateway stays DARK unless the tier is on, and an identity-less request is
// a pristine passthrough that writes NO disable state — otherwise one anonymous
// caller could trip another's breaker.
func TestWireMutateGates(t *testing.T) {
	// Feature off.
	c := wireContainer()
	before := PrintJSONUnformatted(c)
	st := newStats()
	var ctx MutateCtx
	cfg := onCfg()
	cfg.MutateOn = false
	BufferedMutate(c, "messages", "m", "sys", ident(), cfg, wireDeps(&fakeSessions{resolve: true}, st), &ctx)
	if ctx.Mutated || ctx.MutateOn || PrintJSONUnformatted(c) != before {
		t.Error("a disabled tier must not touch the request")
	}

	// Identity-less.
	sess := &fakeSessions{resolve: false}
	ctx = MutateCtx{}
	BufferedMutate(c, "messages", "m", "sys", ident(), onCfg(), wireDeps(sess, st), &ctx)
	if ctx.Mutated || ctx.HaveKey {
		t.Error("an identity-less request must pass through pristine")
	}
	if len(sess.disables) != 0 {
		t.Error("an identity-less request must write NO disable state")
	}

	// Breaker already tripped.
	sess = &fakeSessions{key: "k", resolve: true, disabled: true}
	st = newStats()
	ctx = MutateCtx{}
	BufferedMutate(c, "messages", "m", "sys", ident(), onCfg(), wireDeps(sess, st), &ctx)
	if ctx.Mutated {
		t.Error("a disabled session must pass through pristine")
	}
	if st.counts[StatSessionDisabledBlocks] != 1 {
		t.Errorf("the block should be counted: %v", st.counts)
	}
}

// 4xx: the reduction may be why the provider refused — restore, trip the
// breaker, and resend ONCE.
func TestWireAfterStatus4xxRestoresAndResends(t *testing.T) {
	c := wireContainer()
	pristine := PrintJSONUnformatted(c.Get("messages"))
	sess := &fakeSessions{key: "k1", resolve: true}
	st := newStats()
	var ctx MutateCtx
	BufferedMutate(c, "messages", "m", "sys", ident(), onCfg(), wireDeps(sess, st), &ctx)
	if !ctx.Mutated {
		t.Fatal("setup: expected a mutation")
	}

	action := BufferedAfterStatus(c, "messages", 400, wireDeps(sess, st), &ctx)
	if action != PostResend {
		t.Errorf("got %v, want PostResend", action)
	}
	if PrintJSONUnformatted(c.Get("messages")) != pristine {
		t.Error("the original was not restored byte-for-byte")
	}
	if len(sess.disables) != 1 || !strings.HasSuffix(sess.disables[0], "|4xx") {
		t.Errorf("the breaker should trip with a 4xx reason: %v", sess.disables)
	}
	if st.counts[Stat4xxRestoreResend] != 1 {
		t.Errorf("counter missing: %v", st.counts)
	}
	// The request is pristine now; a second call must not act again.
	if again := BufferedAfterStatus(c, "messages", 400, wireDeps(sess, st), &ctx); again != PostNone {
		t.Error("post-status must not double-handle")
	}
}

// 5xx: provider state is uncertain, so trip the breaker but do NOT resend —
// resending after an ambiguous server error risks duplicating work the provider
// already did.
func TestWireAfterStatus5xxDisablesWithoutResend(t *testing.T) {
	c := wireContainer()
	sess := &fakeSessions{key: "k1", resolve: true}
	st := newStats()
	var ctx MutateCtx
	BufferedMutate(c, "messages", "m", "sys", ident(), onCfg(), wireDeps(sess, st), &ctx)
	if !ctx.Mutated {
		t.Fatal("setup: expected a mutation")
	}
	if action := BufferedAfterStatus(c, "messages", 503, wireDeps(sess, st), &ctx); action != PostNone {
		t.Errorf("got %v, want PostNone", action)
	}
	if len(sess.disables) != 1 || !strings.HasSuffix(sess.disables[0], "|5xx") {
		t.Errorf("breaker: %v", sess.disables)
	}
	if st.counts[Stat5xxDisable] != 1 {
		t.Errorf("counter missing: %v", st.counts)
	}
}

// A 2xx leaves everything alone.
func TestWireAfterStatusSuccessIsInert(t *testing.T) {
	c := wireContainer()
	sess := &fakeSessions{key: "k1", resolve: true}
	st := newStats()
	var ctx MutateCtx
	BufferedMutate(c, "messages", "m", "sys", ident(), onCfg(), wireDeps(sess, st), &ctx)
	if action := BufferedAfterStatus(c, "messages", 200, wireDeps(sess, st), &ctx); action != PostNone {
		t.Errorf("got %v", action)
	}
	if len(sess.disables) != 0 {
		t.Error("success must not trip the breaker")
	}
}

func TestWireStreamDisableOncePerTurn(t *testing.T) {
	sess := &fakeSessions{key: "k1", resolve: true}
	st := newStats()
	ctx := MutateCtx{Mutated: true, HaveKey: true, Key: "k1", TTLMS: 1000}
	StreamDisable(wireDeps(sess, st), &ctx, "")
	if len(sess.disables) != 1 || !strings.HasSuffix(sess.disables[0], "|stream") {
		t.Errorf("default reason: %v", sess.disables)
	}
	// A later frame no-ops.
	StreamDisable(wireDeps(sess, st), &ctx, "later")
	if len(sess.disables) != 1 {
		t.Error("one disable per turn")
	}
}

// EXACT type match: an error type that merely contains these words must not
// false-trip, and throttling must never disable the lever.
func TestWireAnthropicStreamErrorClassification(t *testing.T) {
	yes := []string{
		`{"error":{"type":"invalid_request_error"}}`,
		`{"error":{"type":"request_too_large"}}`,
	}
	no := []string{
		`{"error":{"type":"rate_limit_error"}}`,
		`{"error":{"type":"overloaded_error"}}`,
		`{"error":{"type":"api_error"}}`,
		`{"error":{"type":"authentication_error"}}`,
		`{"error":{"type":"future_invalid_request_error_variant"}}`,
		`{"error":{}}`, `{}`, `not json`, ``,
	}
	for _, s := range yes {
		if !AnthropicStreamErrorIsInvalidRequest(s) {
			t.Errorf("%s should be an invalid-request error", s)
		}
	}
	for _, s := range no {
		if AnthropicStreamErrorIsInvalidRequest(s) {
			t.Errorf("%s must NOT be treated as a reduction bug", s)
		}
	}
}

// Only the statuses a bad reduced serialization can produce. Auth, not-found and
// rate-limit must not disable the lever, or ordinary throttling would switch the
// economizer off.
func TestWireStatusClassification(t *testing.T) {
	for _, s := range []int{400, 413, 422} {
		if !StatusIsInvalidRequest(s) {
			t.Errorf("%d should count as an invalid request", s)
		}
	}
	for _, s := range []int{401, 403, 404, 429, 500, 200} {
		if StatusIsInvalidRequest(s) {
			t.Errorf("%d must not count as a reduction bug", s)
		}
	}
}

// A bypass leaves the container untouched and records why.
func TestWireBypassLeavesContainerPristine(t *testing.T) {
	c := NewObject()
	msgs := NewArray()
	msgs.Append(mkUser("short")) // nothing to compress
	c.Set("messages", msgs)
	before := PrintJSONUnformatted(c)

	sess := &fakeSessions{key: "k1", resolve: true}
	st := newStats()
	var ctx MutateCtx
	BufferedMutate(c, "messages", "m", "sys", ident(), onCfg(), wireDeps(sess, st), &ctx)

	if ctx.Mutated {
		t.Error("nothing was compressible; there should be no mutation")
	}
	if PrintJSONUnformatted(c) != before {
		t.Error("a bypass must leave the container byte-identical")
	}
	if st.counts[StatHardBypass] == 0 || len(st.reasons) == 0 {
		t.Errorf("the bypass reason should be recorded: %v %v", st.counts, st.reasons)
	}
}
