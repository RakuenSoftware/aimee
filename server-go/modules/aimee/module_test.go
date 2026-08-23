package aimee

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
)

func newModule(t *testing.T, ids ...string) *Module {
	t.Helper()
	return moduleOver(t, newRegistry(t, ids...))
}

func newRegistry(t *testing.T, ids ...string) *peer.Registry {
	t.Helper()
	r := peer.New(peer.Options{})
	for _, id := range ids {
		if err := r.Register(id, "uid:1000", "cli"); err != nil {
			t.Fatalf("register %s: %v", id, err)
		}
	}
	return r
}

func moduleOver(t *testing.T, r *peer.Registry) *Module {
	t.Helper()
	m, err := New(NewPeer(r, nil))
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return m
}

// peerRegistry reaches the registry behind the module's peer capability, for
// tests that need to drive it directly.
func (m *Module) peerRegistry() *peer.Registry {
	return m.byStage[peerwire.StageDelivery].(*PeerCapability).registry
}

func call(t *testing.T, m *Module, stage, op uint32, cells []string) (peerwire.Status, []string) {
	t.Helper()
	frame, err := peerwire.EncodeRequest(op, cells)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, st := m.Handle(bus.ModuleInvocation{StageID: stage}, frame)
	if st != bus.ModuleStatusOK {
		t.Fatalf("transport status = %v; want OK (a domain refusal is still a served call)", st)
	}
	status, out, err := peerwire.DecodeResponse(body)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return status, out
}

// The stages this module advertises must match what process-contracts.json
// declares for `aimee`. A stage declared and not advertised is never available:
// every call returns CAPABILITY_ABSENT while the module runs normally, which
// reads as "the module is down" when it plainly is not.
func TestAdvertisedStagesMatchTheContract(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "..", "src", "modules", "process-contracts.json"))
	if err != nil {
		t.Fatalf("read process-contracts.json: %v", err)
	}
	var contracts struct {
		Components []struct {
			ID           string `json:"id"`
			Runtime      string `json:"runtime"`
			PrincipalRef uint32 `json:"principal_ref"`
			Stages       []struct {
				EventKind uint32 `json:"event_kind"`
				ID        uint32 `json:"id"`
				Name      string `json:"name"`
			} `json:"stages"`
		} `json:"components"`
	}
	if err := json.Unmarshal(raw, &contracts); err != nil {
		t.Fatalf("parse: %v", err)
	}

	var declared *struct {
		ID           string `json:"id"`
		Runtime      string `json:"runtime"`
		PrincipalRef uint32 `json:"principal_ref"`
		Stages       []struct {
			EventKind uint32 `json:"event_kind"`
			ID        uint32 `json:"id"`
			Name      string `json:"name"`
		} `json:"stages"`
	}
	for i := range contracts.Components {
		if contracts.Components[i].ID == "aimee" {
			declared = &contracts.Components[i]
			break
		}
	}
	if declared == nil {
		t.Fatal("no `aimee` component in process-contracts.json — this test would pass vacuously")
	}
	if declared.PrincipalRef != PrincipalRef {
		t.Fatalf("contract ref %d != code ref %d", declared.PrincipalRef, PrincipalRef)
	}
	if declared.Runtime != "go" {
		t.Errorf("runtime = %q; want go", declared.Runtime)
	}

	advertised := map[uint32]uint32{}
	for _, s := range newModule(t).Stages() {
		advertised[s.EventKind] = s.StageID
	}
	if len(advertised) != len(declared.Stages) {
		t.Errorf("advertises %d stages, contract declares %d", len(advertised), len(declared.Stages))
	}
	for _, s := range declared.Stages {
		stage, ok := advertised[s.EventKind]
		if !ok {
			t.Errorf("declares %s (event %d) but never advertises it", s.Name, s.EventKind)
			continue
		}
		if stage != s.ID {
			t.Errorf("%s: advertises stage %d, contract says %d", s.Name, stage, s.ID)
		}
		if want := peerwire.EventKind(PrincipalRef, s.ID); want != s.EventKind {
			t.Errorf("%s: contract kind %d breaks the bus formula (want %d)", s.Name, s.EventKind, want)
		}
	}
}

func TestDeliveryStageSendAndInbox(t *testing.T) {
	m := newModule(t, "A", "B")

	status, cells := call(t, m, peerwire.StageDelivery, peerwire.OpSend, []string{"A", "B", "hello", "", "0", "0"})
	if status != peerwire.StatusOK {
		t.Fatalf("send status = %v", status)
	}
	sent, err := peerwire.MessageRows(cells)
	if err != nil || len(sent) != 1 {
		t.Fatalf("send reply: %v (%d rows)", err, len(sent))
	}
	// Provenance is stamped by the registry, not taken from the caller's cells.
	if sent[0].FromSession != "A" || sent[0].FromOwner != "uid:1000" {
		t.Errorf("provenance = %+v", sent[0])
	}

	status, cells = call(t, m, peerwire.StageInbox, peerwire.OpInboxLen, []string{"B"})
	if status != peerwire.StatusOK || len(cells) != 2 || cells[0] != "1" || cells[1] != "0" {
		t.Fatalf("inbox len = %v %q", status, cells)
	}

	// Peek must not consume.
	if _, cells = call(t, m, peerwire.StageInbox, peerwire.OpInboxPeek, []string{"B"}); len(cells) != peerwire.MessageWidth {
		t.Fatalf("peek cells = %d", len(cells))
	}
	if _, cells = call(t, m, peerwire.StageInbox, peerwire.OpInboxLen, []string{"B"}); cells[0] != "1" {
		t.Error("peek consumed the inbox; it must only read")
	}

	// Take removes what it returns, and says how many are left behind so a
	// caller can tell a complete drain from a capped one.
	_, cells = call(t, m, peerwire.StageInbox, peerwire.OpInboxTake, []string{"B", "10"})
	remaining, got, err := peerwire.TakeReply(cells)
	if err != nil || len(got) != 1 || got[0].Text != "hello" {
		t.Fatalf("take = %v %+v", err, got)
	}
	if remaining != 0 {
		t.Errorf("remaining = %d; want 0 (that was the whole inbox)", remaining)
	}
	if _, cells = call(t, m, peerwire.StageInbox, peerwire.OpInboxLen, []string{"B"}); cells[0] != "0" {
		t.Error("take did not remove what it returned")
	}
}

// A domain refusal is a SUCCESSFUL invocation carrying a policy outcome. If it
// arrived as a transport error the governance tap could not tell "the module is
// broken" from "the module said no".
func TestRefusalsAreServedNotTransportErrors(t *testing.T) {
	m := newModule(t, "A", "B")

	for name, tc := range map[string]struct {
		cells []string
		want  peerwire.Status
	}{
		"unknown peer": {[]string{"A", "ghost", "hi", "", "0", "0"}, peerwire.StatusNoPeer},
		"self":         {[]string{"A", "A", "hi", "", "0", "0"}, peerwire.StatusSelf},
		"empty body":   {[]string{"A", "B", "   ", "", "0", "0"}, peerwire.StatusBadRequest},
	} {
		// call() already fails the test if the transport status is not OK.
		if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpSend, tc.cells); status != tc.want {
			t.Errorf("%s: status = %v; want %v", name, status, tc.want)
		}
	}
}

// An expecting send records the wait-for edge server-side, so a peer asking back
// is refused as a cycle — without any handler ever blocking.
func TestExpectReplyRecordsTheEdgeWithoutBlocking(t *testing.T) {
	m := newModule(t, "A", "B")

	if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpSend,
		[]string{"A", "B", "you first", "", "0", "1"}); status != peerwire.StatusOK {
		t.Fatalf("expecting send = %v", status)
	}
	// B asking back closes A→B→A.
	if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpSend,
		[]string{"B", "A", "no, you", "", "1", "1"}); status != peerwire.StatusCycle {
		t.Fatalf("counter-ask = %v; want cycle", status)
	}
	// The non-blocking fallback still works: a plain send is not an ask.
	if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpSend,
		[]string{"B", "A", "fallback", "", "1", "0"}); status != peerwire.StatusOK {
		t.Errorf("fallback send = %v", status)
	}
	// Cancelling the edge makes the path askable again.
	if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpCancelWait, []string{"A"}); status != peerwire.StatusOK {
		t.Fatalf("cancel = %v", status)
	}
	if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpSend,
		[]string{"B", "A", "now?", "", "1", "1"}); status != peerwire.StatusOK {
		t.Errorf("after cancel = %v; want the cycle to be gone", status)
	}
}

func TestGrantStage(t *testing.T) {
	r := peer.New(peer.Options{})
	if err := r.Register("A", "uid:1000", "cli"); err != nil {
		t.Fatal(err)
	}
	if err := r.Register("X", "uid:2000", "cli"); err != nil {
		t.Fatal(err)
	}
	m := moduleOver(t, r)

	if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpSend,
		[]string{"A", "X", "hi", "", "0", "0"}); status != peerwire.StatusDenied {
		t.Fatalf("ungranted = %v; want denied", status)
	}
	if status, _ := call(t, m, peerwire.StageGrant, peerwire.OpGrant, []string{"uid:1000", "uid:2000"}); status != peerwire.StatusOK {
		t.Fatal("grant refused")
	}
	if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpSend,
		[]string{"A", "X", "hi", "", "0", "0"}); status != peerwire.StatusOK {
		t.Fatal("granted send refused")
	}
	// Directed: the reverse is a separate grant. Note this answers peerwire.StatusOK with
	// a false value rather than refusing — see TestThreeOutcomeLevelsStayDistinct.
	_, cells := call(t, m, peerwire.StageGrant, peerwire.OpGrantExists, []string{"uid:2000", "uid:1000"})
	if len(cells) != 1 || cells[0] != "0" {
		t.Errorf("reverse grant = %q; a grant must be directed", cells)
	}
	_, cells = call(t, m, peerwire.StageGrant, peerwire.OpRevoke, []string{"uid:1000", "uid:2000"})
	if len(cells) != 1 || cells[0] != "1" {
		t.Errorf("revoke = %q", cells)
	}
}

// Malformed input is a TRANSPORT-level invalid request, distinct from a domain
// refusal: the module could not understand the question at all.
func TestMalformedInputIsInvalidRequest(t *testing.T) {
	m := newModule(t, "A", "B")

	for name, req := range map[string][]byte{
		"garbage":   []byte("not a frame"),
		"truncated": {1, 0, 0, 0},
	} {
		if _, st := m.Handle(bus.ModuleInvocation{StageID: peerwire.StageDelivery}, req); st != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s: status = %v; want InvalidRequest", name, st)
		}
	}

	// Wrong cell count for the operation.
	frame, _ := peerwire.EncodeRequest(peerwire.OpSend, []string{"A"})
	if _, st := m.Handle(bus.ModuleInvocation{StageID: peerwire.StageDelivery}, frame); st != bus.ModuleStatusInvalidRequest {
		t.Errorf("short cells: status = %v; want InvalidRequest", st)
	}
	// Unknown stage.
	frame, _ = peerwire.EncodeRequest(peerwire.OpSend, []string{"A", "B", "x", "", "0", "0"})
	if _, st := m.Handle(bus.ModuleInvocation{StageID: 99}, frame); st != bus.ModuleStatusInvalidRequest {
		t.Errorf("unknown stage: status = %v; want InvalidRequest", st)
	}
	// Unknown op within a known stage.
	frame, _ = peerwire.EncodeRequest(99, []string{"A"})
	if _, st := m.Handle(bus.ModuleInvocation{StageID: peerwire.StageInbox}, frame); st != bus.ModuleStatusInvalidRequest {
		t.Errorf("unknown op: status = %v; want InvalidRequest", st)
	}
}

// The three outcome levels must stay distinguishable, because a governance tap
// that sees a steady rate of non-OK cannot tell "policy is working as designed"
// from "something is wrong".
//
//	ModuleStatus non-OK       could not understand the question
//	domain status non-OK      understood, and refuses
//	peerwire.StatusOK + outcome value  understood, and the answer is no
func TestThreeOutcomeLevelsStayDistinct(t *testing.T) {
	m := newModule(t, "A", "B")

	// "the answer is no" -- a grant that does not exist is not a refusal. Asking
	// is a legitimate question with a legitimate negative answer, and it is
	// itself the audit event that records someone asking.
	status, cells := call(t, m, peerwire.StageGrant, peerwire.OpGrantExists, []string{"uid:1000", "uid:9999"})
	if status != peerwire.StatusOK {
		t.Errorf("absent grant: status = %v; want ok with a false value", status)
	}
	if len(cells) != 1 || cells[0] != "0" {
		t.Errorf("absent grant cells = %q; want a false value", cells)
	}

	// "the answer is none" -- an empty inbox on a live session is not a refusal.
	// The reply still leads with a remaining count, which is zero.
	if status, cells = call(t, m, peerwire.StageInbox, peerwire.OpInboxTake, []string{"B", "10"}); status != peerwire.StatusOK {
		t.Errorf("empty inbox: status = %v; want ok", status)
	} else if remaining, msgs, err := peerwire.TakeReply(cells); err != nil || remaining != 0 || len(msgs) != 0 {
		t.Errorf("empty inbox = remaining %d, %d msgs, err %v; want 0/0/nil", remaining, len(msgs), err)
	}

	// "I refuse" -- a send that did not happen, where the caller must do
	// something differently to make it happen.
	if status, _ = call(t, m, peerwire.StageDelivery, peerwire.OpSend,
		[]string{"A", "ghost", "hi", "", "0", "0"}); status != peerwire.StatusNoPeer {
		t.Errorf("send to unknown peer: status = %v; want a refusal", status)
	}

	// "could not understand" -- transport level, never a domain status.
	if _, st := m.Handle(bus.ModuleInvocation{StageID: peerwire.StageInbox}, []byte("garbage")); st == bus.ModuleStatusOK {
		t.Error("an undecodable frame answered OK; it must fail at the transport level")
	}
}

// Reading the inbox of a session that does not exist is a REFUSAL, not an empty
// inbox. The two are indistinguishable as a count, and answering OK-with-zero is
// how a caller polling for a reply waits forever on a session that was torn
// down -- reading zero and going round again instead of failing fast.
func TestUnknownSessionIsNotAnEmptyInbox(t *testing.T) {
	m := newModule(t, "A", "B")

	for _, tc := range []struct {
		name  string
		op    uint32
		cells []string
	}{
		{"len", peerwire.OpInboxLen, []string{"ghost"}},
		{"peek", peerwire.OpInboxPeek, []string{"ghost"}},
		{"take", peerwire.OpInboxTake, []string{"ghost", "10"}},
	} {
		if status, _ := call(t, m, peerwire.StageInbox, tc.op, tc.cells); status != peerwire.StatusNoPeer {
			t.Errorf("%s on unknown session: status = %v; want no_peer", tc.name, status)
		}
	}

	// A live session with an empty inbox still answers OK, so the refusal above
	// is about existence and not about emptiness.
	if status, cells := call(t, m, peerwire.StageInbox, peerwire.OpInboxLen, []string{"B"}); status != peerwire.StatusOK ||
		len(cells) != 2 || cells[0] != "0" {
		t.Errorf("live empty inbox = %v %q; want ok with a zero count", status, cells)
	}

	// The case that motivates all of this: a session torn down mid-poll must
	// become distinguishable from one that simply has no mail.
	if status, _ := call(t, m, peerwire.StageInbox, peerwire.OpInboxLen, []string{"A"}); status != peerwire.StatusOK {
		t.Fatalf("precondition: A should be live, got %v", status)
	}
	m.peerRegistry().Unregister("A")
	if status, _ := call(t, m, peerwire.StageInbox, peerwire.OpInboxLen, []string{"A"}); status != peerwire.StatusNoPeer {
		t.Errorf("after teardown: status = %v; a poller must learn its session is gone", status)
	}
}

// fakeCapability advertises whatever stages it is given.
type fakeCapability struct{ stages []bus.ModuleStage }

func (f fakeCapability) Stages() []bus.ModuleStage { return f.stages }
func (f fakeCapability) Handle(bus.ModuleInvocation, []byte) ([]byte, bus.ModuleStatus) {
	return nil, bus.ModuleStatusOK
}

// Two capabilities claiming one stage must fail at CONSTRUCTION. Left to
// runtime, the loser is silently unreachable: its stage IS advertised, so calls
// arrive and are served by the wrong owner, which returns plausible nonsense.
// That is worse than CAPABILITY_ABSENT, which at least fails visibly.
func TestStageConflictRefusedAtConstruction(t *testing.T) {
	clash := fakeCapability{stages: []bus.ModuleStage{
		{EventKind: peerwire.EventKind(PrincipalRef, peerwire.StageInbox), StageID: peerwire.StageInbox},
	}}
	_, err := New(NewPeer(newRegistry(t), nil), clash)
	if !errors.Is(err, ErrStageConflict) {
		t.Fatalf("duplicate stage: err = %v; want ErrStageConflict", err)
	}
}

// A stage whose event kind does not follow the bus formula is refused too. The
// kind and the principal ref are one fact; a hand-written kind that disagrees
// with the formula is how a module advertises something the contract does not
// declare.
func TestStageWithOffFormulaKindRefused(t *testing.T) {
	bad := fakeCapability{stages: []bus.ModuleStage{
		{EventKind: 9999, StageID: 7},
	}}
	if _, err := New(bad); err == nil {
		t.Fatal("a stage whose kind breaks the bus formula was accepted")
	}
}

// The module is a host for capabilities, not peer messaging in a wrapper. A
// second capability is a New() argument.
func TestModuleHostsMultipleCapabilities(t *testing.T) {
	second := fakeCapability{stages: []bus.ModuleStage{
		{EventKind: peerwire.EventKind(PrincipalRef, 9), StageID: 9},
	}}
	m, err := New(NewPeer(newRegistry(t, "A", "B"), nil), second)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	// Derived, not transcribed: how many stages peer messaging serves is its own
	// business and grows when it gains one. A literal here fails every time that
	// happens, which says nothing about whether hosting works.
	wantStages := len(NewPeer(newRegistry(t), nil).Stages()) + 1
	if got := len(m.Stages()); got != wantStages {
		t.Fatalf("stages = %d; want %d (peer's own, plus one other)", got, wantStages)
	}
	// Each stage reaches its own owner.
	if _, st := m.Handle(bus.ModuleInvocation{StageID: 9}, nil); st != bus.ModuleStatusOK {
		t.Errorf("second capability stage = %v; want OK", st)
	}
	// And peer messaging still works alongside it.
	if status, _ := call(t, m, peerwire.StageDelivery, peerwire.OpSend,
		[]string{"A", "B", "still here", "", "0", "0"}); status != peerwire.StatusOK {
		t.Errorf("peer send alongside a second capability = %v", status)
	}
	// A nil capability is ignored rather than panicking, so a caller can pass
	// one conditionally without branching.
	if _, err := New(NewPeer(newRegistry(t), nil), nil); err != nil {
		t.Errorf("nil capability: %v", err)
	}
}
