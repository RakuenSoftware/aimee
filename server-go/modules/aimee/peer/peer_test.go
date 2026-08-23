package peer

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"
	"unicode/utf8"
)

// newTestRegistry builds a registry with two same-owner peers registered, and
// records live notifications so tests can assert peer traffic is observable.
func newTestRegistry(t *testing.T, ids ...string) (*Registry, *noteLog) {
	t.Helper()
	nl := &noteLog{}
	r := New(Options{Notify: nl.record})
	for _, id := range ids {
		if err := r.Register(id, "uid:1000", "cli"); err != nil {
			t.Fatalf("Register(%q): %v", id, err)
		}
	}
	return r, nl
}

type noteLog struct {
	mu    sync.Mutex
	notes []struct {
		to string
		m  Message
	}
}

func (n *noteLog) record(to string, m Message) {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.notes = append(n.notes, struct {
		to string
		m  Message
	}{to, m})
}

func (n *noteLog) forSession(id string) []Message {
	n.mu.Lock()
	defer n.mu.Unlock()
	var out []Message
	for _, x := range n.notes {
		if x.to == id {
			out = append(out, x.m)
		}
	}
	return out
}

// A1 — the directory resolves a peer by label, and a delivered message carries
// provenance the receiver did not author and the sender could not forge.
func TestDirectoryAndProvenance(t *testing.T) {
	r, notes := newTestRegistry(t, "A", "B")

	if err := r.SetLabel("A", "author"); err != nil {
		t.Fatalf("SetLabel A: %v", err)
	}
	if err := r.SetLabel("B", "reviewer"); err != nil {
		t.Fatalf("SetLabel B: %v", err)
	}

	got, ok := r.Lookup("uid:1000", "reviewer")
	if !ok || got != "B" {
		t.Fatalf("Lookup(reviewer) = %q,%v; want B,true", got, ok)
	}
	// Uniqueness per owner is what keeps the directory honest about who
	// "reviewer" is.
	if err := r.SetLabel("A", "reviewer"); !errors.Is(err, ErrLabelTaken) {
		t.Fatalf("duplicate label: err = %v; want ErrLabelTaken", err)
	}
	if _, ok := r.Lookup("uid:1000", "nobody"); ok {
		t.Fatal("Lookup(nobody) succeeded")
	}

	dir := r.Directory("uid:1000")
	if len(dir) != 2 {
		t.Fatalf("Directory = %d entries; want 2", len(dir))
	}

	m, err := r.Send("A", "B", "look at this diff", SendOptions{})
	if err != nil {
		t.Fatalf("Send: %v", err)
	}

	got2 := r.Take("B", 1)
	if len(got2) != 1 {
		t.Fatalf("Take = %d; want 1", len(got2))
	}
	rcv := got2[0]
	if rcv.Text != "look at this diff" {
		t.Errorf("Text = %q", rcv.Text)
	}
	// Provenance comes from the sender's directory entry, not from an argument.
	if rcv.FromSession != "A" || rcv.FromOwner != "uid:1000" || rcv.FromLabel != "author" {
		t.Errorf("provenance = %+v", rcv)
	}
	if rcv.OriginSession != "A" {
		t.Errorf("OriginSession = %q; want A", rcv.OriginSession)
	}
	if rcv.ID != m.ID || rcv.ConversationID == "" {
		t.Errorf("ids = %q/%q", rcv.ID, rcv.ConversationID)
	}
	if rcv.Hop != 0 || rcv.IsReply {
		t.Errorf("hop/isReply = %d/%v; want 0/false", rcv.Hop, rcv.IsReply)
	}
	if r.Len("B") != 0 {
		t.Errorf("inbox not drained")
	}

	// The message was announced live, so an attached human sees peer traffic
	// as it happens rather than only in an audit query.
	seen := notes.forSession("B")
	if len(seen) != 1 || seen[0].FromLabel != "author" {
		t.Fatalf("notifications = %+v", seen)
	}
	if seen[0].Preview() != "look at this diff" {
		t.Errorf("preview = %q", seen[0].Preview())
	}
}

// Preview is an excerpt, never a substitute for the message: over-long bodies
// are refused outright rather than silently truncated.
func TestNoSilentTruncation(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")

	long := strings.Repeat("x", MaxTextBytes+1)
	if _, err := r.Send("A", "B", long, SendOptions{}); !errors.Is(err, ErrTooLong) {
		t.Fatalf("over-long Send: err = %v; want ErrTooLong", err)
	}
	if r.Len("B") != 0 {
		t.Fatal("a refused message was delivered anyway")
	}

	ok := strings.Repeat("y", PreviewBytes+10)
	m, err := r.Send("A", "B", ok, SendOptions{})
	if err != nil {
		t.Fatalf("Send: %v", err)
	}
	if len(m.Preview()) != PreviewBytes {
		t.Errorf("Preview len = %d; want %d", len(m.Preview()), PreviewBytes)
	}
	if got := r.Take("B", 1); got[0].Text != ok {
		t.Error("inbox text was truncated; it must stay authoritative")
	}
}

// A2 — a message arriving while the receiver is mid-turn neither preempts nor
// races that turn. Delivery is pull-based, so the receiver decides when.
func TestDeliveryDoesNotPreempt(t *testing.T) {
	// The turn lock is C-owned; this stubs the read across that boundary.
	var turnLive bool
	r := New(Options{Live: func(string) (LiveState, bool) {
		return LiveState{Attachments: 1, TurnInFlight: turnLive, TurnID: "turn-1"}, true
	}})
	for _, id := range []string{"A", "B"} {
		if err := r.Register(id, "uid:1000", "cli"); err != nil {
			t.Fatal(err)
		}
	}

	turnLive = true // B is mid-turn
	if _, err := r.Send("A", "B", "while you're busy", SendOptions{}); err != nil {
		t.Fatalf("Send: %v", err)
	}

	// The turn is untouched and the message simply waits.
	for _, e := range r.Directory("uid:1000") {
		if e.SessionID == "B" && (!e.TurnInFlight || e.TurnID != "turn-1") {
			t.Errorf("turn state disturbed: %+v", e)
		}
	}
	if r.Len("B") != 1 {
		t.Fatalf("inbox = %d; want 1 message waiting", r.Len("B"))
	}

	turnLive = false // turn ends; B drains on its own terms
	if got := r.Take("B", 10); len(got) != 1 || got[0].Text != "while you're busy" {
		t.Fatalf("drain = %+v", got)
	}
}

// A3 — a message to a session with no live attachment survives to be delivered
// when it comes back. The pending inbox is what keeps the entry alive.
func TestMessageSurvivesUnregister(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")

	if _, err := r.Send("A", "B", "read me later", SendOptions{}); err != nil {
		t.Fatalf("Send: %v", err)
	}
	r.Unregister("B")

	if r.Len("B") != 1 {
		t.Fatal("message dropped when the receiver detached")
	}
	if err := r.Register("B", "uid:1000", "cli"); err != nil {
		t.Fatalf("re-Register: %v", err)
	}
	if got := r.Take("B", 1); len(got) != 1 || got[0].Text != "read me later" {
		t.Fatalf("after re-attach: %+v", got)
	}

	// A session that detaches with an empty inbox is really gone.
	r.Unregister("B")
	if _, err := r.Send("A", "B", "hello?", SendOptions{}); !errors.Is(err, ErrNoPeer) {
		t.Fatalf("send to gone session: err = %v; want ErrNoPeer", err)
	}
}

// replyOnce drains one message and answers it.
func replyOnce(t *testing.T, r *Registry, me, text string) <-chan error {
	t.Helper()
	done := make(chan error, 1)
	go func() {
		deadline := time.Now().Add(3 * time.Second)
		for time.Now().Before(deadline) {
			if got := r.Take(me, 1); len(got) == 1 {
				_, err := r.Reply(me, got[0], text)
				done <- err
				return
			}
			time.Sleep(2 * time.Millisecond)
		}
		done <- errors.New("no message arrived")
	}()
	return done
}

// A4 — Ask returns the peer's reply; a timed-out Ask degrades to a Send and the
// late reply still arrives, correlated.
func TestAskAndTimeout(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")

	done := replyOnce(t, r, "B", "looks good")
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	reply, askID, err := r.Ask(ctx, "A", "B", "ship it?")
	if err != nil {
		t.Fatalf("Ask: %v", err)
	}
	if err := <-done; err != nil {
		t.Fatalf("replier: %v", err)
	}
	if reply.Text != "looks good" || reply.FromSession != "B" {
		t.Errorf("reply = %+v", reply)
	}
	if reply.CorrelationID != askID {
		t.Errorf("correlation = %q; want %q", reply.CorrelationID, askID)
	}
	if !reply.IsReply || reply.Hop != 1 {
		t.Errorf("isReply/hop = %v/%d; want true/1", reply.IsReply, reply.Hop)
	}
	// The Ask consumed the reply; it must not also be drained later.
	if r.Len("A") != 0 {
		t.Errorf("asker inbox = %d; want 0", r.Len("A"))
	}

	// --- the timed-out ask: degrades to a send, loses nothing ---
	ctx2, cancel2 := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel2()
	_, askID2, err := r.Ask(ctx2, "A", "B", "and this?")
	if !errors.Is(err, ErrTimeout) {
		t.Fatalf("Ask: err = %v; want ErrTimeout", err)
	}
	if askID2 == "" {
		t.Fatal("a timed-out ask must still report the question it sent")
	}
	if r.Len("B") != 1 {
		t.Fatal("the question did not stay in the peer's inbox")
	}

	if err := <-replyOnce(t, r, "B", "late answer"); err != nil {
		t.Fatalf("late replier: %v", err)
	}
	late := r.Take("A", 1)
	if len(late) != 1 || late[0].Text != "late answer" {
		t.Fatalf("late reply = %+v", late)
	}
	// It still carries the ORIGINAL correlation id: recoverable, not lost.
	if late[0].CorrelationID != askID2 {
		t.Errorf("late correlation = %q; want %q", late[0].CorrelationID, askID2)
	}
}

// A5 — mutual asks are refused with ErrCycle rather than deadlocking, and
// neither session is left wedged. Driven as a real race.
func TestAskCycleRefused(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B", "C")

	// A asks B and blocks.
	askErr := make(chan error, 1)
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 1500*time.Millisecond)
		defer cancel()
		_, _, err := r.Ask(ctx, "A", "B", "you first")
		askErr <- err
	}()

	deadline := time.Now().Add(2 * time.Second)
	for r.Len("B") == 0 && time.Now().Before(deadline) {
		time.Sleep(2 * time.Millisecond)
	}
	if r.Len("B") != 1 {
		t.Fatal("A's ask never reached B")
	}

	// B asking A back would close the cycle — refused immediately, not blocked.
	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, _, err := r.Ask(ctx, "B", "A", "no, you"); !errors.Is(err, ErrCycle) {
		t.Fatalf("counter-ask: err = %v; want ErrCycle", err)
	}
	if elapsed := time.Since(start); elapsed > time.Second {
		t.Errorf("cycle refusal blocked for %v; it must be immediate", elapsed)
	}

	// The refusal did not wedge B: the non-blocking fallback still works.
	if _, err := r.Send("B", "A", "fallback", SendOptions{Hop: 1}); err != nil {
		t.Errorf("fallback Send: %v", err)
	}

	if err := <-askErr; !errors.Is(err, ErrTimeout) {
		t.Fatalf("A's ask = %v; want ErrTimeout (it must not hang)", err)
	}

	// Self-addressing is the smallest cycle of all.
	if _, _, err := r.Ask(ctx, "A", "A", "hello me"); !errors.Is(err, ErrCycle) {
		t.Errorf("self-ask: err = %v; want ErrCycle", err)
	}
}

// A transitive cycle (A→B→C→A) is refused too, not just the direct one.
func TestAskCycleTransitive(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B", "C")

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); r.Ask(ctx, "A", "B", "q") }()
	go func() { defer wg.Done(); r.Ask(ctx, "B", "C", "q") }()

	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if r.Len("B") > 0 && r.Len("C") > 0 {
			break
		}
		time.Sleep(2 * time.Millisecond)
	}

	// C asking A closes A→B→C→A.
	cctx, ccancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer ccancel()
	if _, _, err := r.Ask(cctx, "C", "A", "closing the loop"); !errors.Is(err, ErrCycle) {
		t.Fatalf("transitive counter-ask: err = %v; want ErrCycle", err)
	}
	wg.Wait()
}

// A6 — a ping-pong terminates at the hop ceiling, and the origin session rides
// every hop so the exchange is charged to whoever started it.
func TestHopBudgetTerminatesPingPong(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	r.SetMaxHops(4)
	if r.MaxHops() != 4 {
		t.Fatalf("MaxHops = %d", r.MaxHops())
	}

	if _, err := r.Send("A", "B", "ping", SendOptions{}); err != nil {
		t.Fatalf("opener: %v", err)
	}

	at, other := "B", "A"
	exchanges := 0
	var last error
	for i := 0; i < 50; i++ {
		got := r.Take(at, 1)
		if len(got) != 1 {
			break
		}
		if got[0].OriginSession != "A" {
			t.Fatalf("origin lost at hop %d: %q", got[0].Hop, got[0].OriginSession)
		}
		_, last = r.Reply(at, got[0], "pong")
		exchanges++
		if last != nil {
			break
		}
		at, other = other, at
	}
	if !errors.Is(last, ErrHopLimit) {
		t.Fatalf("ping-pong ended with %v; want ErrHopLimit", last)
	}
	// hops 1..3 delivered; the 4th is refused at the ceiling.
	if exchanges != 4 {
		t.Errorf("exchanges = %d; want 4", exchanges)
	}
}

// A8 — cross-owner addressing is refused without a grant, grants are directed,
// and revocation takes effect.
func TestCrossOwnerGrants(t *testing.T) {
	r := New(Options{})
	if err := r.Register("A", "uid:1000", "cli"); err != nil {
		t.Fatal(err)
	}
	if err := r.Register("B", "uid:2000", "cli"); err != nil {
		t.Fatal(err)
	}

	if _, err := r.Send("A", "B", "hello", SendOptions{}); !errors.Is(err, ErrDenied) {
		t.Fatalf("ungranted: err = %v; want ErrDenied", err)
	}
	if r.Len("B") != 0 {
		t.Fatal("a denied message was delivered")
	}

	if err := r.Grant("uid:1000", "uid:2000"); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Send("A", "B", "hello", SendOptions{}); err != nil {
		t.Fatalf("granted Send: %v", err)
	}

	// A grant is directed: it says nothing about the reverse.
	if r.GrantExists("uid:2000", "uid:1000") {
		t.Error("grant leaked in the reverse direction")
	}
	if _, err := r.Send("B", "A", "hi back", SendOptions{}); !errors.Is(err, ErrDenied) {
		t.Fatalf("reverse: err = %v; want ErrDenied", err)
	}

	if !r.Revoke("uid:1000", "uid:2000") {
		t.Error("Revoke reported nothing to revoke")
	}
	if _, err := r.Send("A", "B", "again", SendOptions{}); !errors.Is(err, ErrDenied) {
		t.Fatalf("after revoke: err = %v; want ErrDenied", err)
	}
	// The message authorized while the grant stood is still there.
	if r.Len("B") != 1 {
		t.Errorf("inbox = %d; want the one authorized message", r.Len("B"))
	}

	// Same-owner visibility: the directory is filterable by principal.
	if dir := r.Directory("uid:1000"); len(dir) != 1 || dir[0].SessionID != "A" {
		t.Errorf("owner-filtered directory = %+v", dir)
	}
}

// Bounds and error paths: unknown peers, self-addressing, and a full inbox that
// counts its drops rather than losing them silently.
func TestBoundsAndErrors(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")

	if _, err := r.Send("A", "nobody", "?", SendOptions{}); !errors.Is(err, ErrNoPeer) {
		t.Errorf("unknown peer: %v", err)
	}
	if _, err := r.Send("ghost", "B", "?", SendOptions{}); !errors.Is(err, ErrUnknownSender) {
		t.Errorf("unknown sender: %v", err)
	}
	if _, err := r.Send("A", "A", "self", SendOptions{}); !errors.Is(err, ErrSelf) {
		t.Errorf("self send: %v", err)
	}
	if _, err := r.Send("A", "B", "   ", SendOptions{}); !errors.Is(err, ErrBadRequest) {
		t.Errorf("empty body: %v", err)
	}

	for i := 0; i < InboxMax; i++ {
		if _, err := r.Send("A", "B", "fill", SendOptions{}); err != nil {
			t.Fatalf("fill %d: %v", i, err)
		}
	}
	if _, err := r.Send("A", "B", "one too many", SendOptions{}); !errors.Is(err, ErrInboxFull) {
		t.Fatalf("overflow: err = %v; want ErrInboxFull", err)
	}
	if r.Dropped("B") != 1 {
		t.Errorf("Dropped = %d; want 1 — an overflow must never be silent", r.Dropped("B"))
	}

	if got := r.Take("B", 8); len(got) != 8 {
		t.Fatalf("batch drain = %d; want 8", len(got))
	}
	if r.Len("B") != InboxMax-8 {
		t.Errorf("remaining = %d", r.Len("B"))
	}
	for r.Take("B", 8) != nil {
	}
	if r.Len("B") != 0 {
		t.Errorf("inbox not empty after full drain")
	}
}

// Concurrent senders and drainers must not corrupt an inbox or lose a message.
// Run with -race, this is what proves the locking.
func TestConcurrentSendAndDrain(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	r.SetMaxHops(1000)

	const senders, each = 4, 40
	var wg sync.WaitGroup
	var sent, refused int64
	var mu sync.Mutex

	drained := make(chan int, 1)
	stop := make(chan struct{})
	go func() {
		n := 0
		for {
			select {
			case <-stop:
				n += len(r.Take("B", InboxMax))
				drained <- n
				return
			default:
				n += len(r.Take("B", 4))
				time.Sleep(time.Millisecond)
			}
		}
	}()

	wg.Add(senders)
	for i := 0; i < senders; i++ {
		go func() {
			defer wg.Done()
			for j := 0; j < each; j++ {
				_, err := r.Send("A", "B", "concurrent", SendOptions{})
				mu.Lock()
				if err == nil {
					sent++
				} else if errors.Is(err, ErrInboxFull) {
					refused++
				} else {
					t.Errorf("unexpected Send error: %v", err)
				}
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	time.Sleep(20 * time.Millisecond)
	close(stop)
	got := <-drained

	if int64(got) != sent {
		t.Errorf("drained %d but %d were accepted — messages were lost or duplicated", got, sent)
	}
	if sent+refused != senders*each {
		t.Errorf("accounted %d of %d sends", sent+refused, senders*each)
	}
}

// PreviewBytes is a BYTE bound, and a multi-byte character straddling it must
// not be cut in half. Invalid UTF-8 in a live notification travels into a
// reader's context, and peer message bodies are the most likely thing in this
// system to contain multi-byte characters.
//
// This is the bytes-versus-characters confusion that bit db1's length-checked
// columns, in its Go form: len() counts bytes, and slicing at a byte offset
// respects no character boundary.
func TestPreviewNeverSplitsARune(t *testing.T) {
	// Place a 3-byte rune so it straddles the bound at every offset it can.
	for pad := PreviewBytes - 2; pad <= PreviewBytes; pad++ {
		body := strings.Repeat("a", pad) + "\u2713" + strings.Repeat("b", 64)
		got := Message{Text: body}.Preview()
		if !utf8.ValidString(got) {
			t.Errorf("pad %d: preview is not valid UTF-8: %q", pad, got)
		}
		if len(got) > PreviewBytes {
			t.Errorf("pad %d: preview is %d bytes, over the %d bound", pad, len(got), PreviewBytes)
		}
		// Backing off to a boundary may shorten the preview, but never by more
		// than the largest rune minus one.
		if PreviewBytes-len(got) > 3 {
			t.Errorf("pad %d: backed off %d bytes; at most 3 is expected", pad, PreviewBytes-len(got))
		}
		if !strings.HasPrefix(body, got) {
			t.Errorf("pad %d: preview is not a prefix of the body", pad)
		}
	}

	// A body made entirely of multi-byte characters is the same case without an
	// ASCII run to hide behind.
	got := Message{Text: strings.Repeat("\u4e16", 400)}.Preview()
	if !utf8.ValidString(got) {
		t.Errorf("all-multibyte preview is not valid UTF-8")
	}

	// A short body is returned whole, multi-byte or not.
	short := "hello \u2713 world"
	if got := (Message{Text: short}).Preview(); got != short {
		t.Errorf("short body preview = %q; want the whole body", got)
	}
}

// A capped drain and a complete one must be distinguishable. Rows alone cannot
// tell "that was all of it" from "that was the first max of more", and a caller
// that drains once and assumes empty simply stops asking -- the failure corrects
// itself never, because nothing further is requested.
func TestDrainDistinguishesCappedFromComplete(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	for i := 0; i < 10; i++ {
		if _, err := r.Send("A", "B", "msg", SendOptions{}); err != nil {
			t.Fatal(err)
		}
	}

	// Capped: fewer taken than exist, and the count says so.
	got, remaining := r.Drain("B", 4)
	if len(got) != 4 || remaining != 6 {
		t.Fatalf("capped drain = %d taken, %d remaining; want 4/6", len(got), remaining)
	}

	// Complete: asking for more than exist takes them all and reports none left.
	got, remaining = r.Drain("B", 100)
	if len(got) != 6 || remaining != 0 {
		t.Fatalf("complete drain = %d taken, %d remaining; want 6/0", len(got), remaining)
	}

	// Exactly-emptying is COMPLETE, not capped: the boundary case where a naive
	// implementation reports "maybe more" forever.
	for i := 0; i < 3; i++ {
		if _, err := r.Send("A", "B", "msg", SendOptions{}); err != nil {
			t.Fatal(err)
		}
	}
	if got, remaining = r.Drain("B", 3); len(got) != 3 || remaining != 0 {
		t.Fatalf("exact drain = %d taken, %d remaining; want 3/0", len(got), remaining)
	}

	// An empty inbox drains to nothing with nothing left.
	if got, remaining = r.Drain("B", 5); len(got) != 0 || remaining != 0 {
		t.Fatalf("empty drain = %d/%d; want 0/0", len(got), remaining)
	}
}

// The bounds comment claims these ceilings are "the reason a runaway exchange
// between two agents cannot consume the appliance". That claim was FALSE for the
// two collections below: the C registry capped both, the Go port dropped both
// caps, and the prose kept asserting a property the code no longer had.
//
// Refused rather than evicted, in both cases. Evicting to make room would
// discard an inbox somebody is waiting on and choose the victim by map order.
func TestRegistryAndGrantTablesAreBounded(t *testing.T) {
	r := New(Options{})
	for i := 0; i < SessionsMax; i++ {
		if err := r.Register(fmt.Sprintf("s-%d", i), "uid:1000", "cli"); err != nil {
			t.Fatalf("session %d: %v", i, err)
		}
	}
	if err := r.Register("one-too-many", "uid:1000", "cli"); !errors.Is(err, ErrRegistryFull) {
		t.Fatalf("session ceiling: err = %v; want ErrRegistryFull", err)
	}
	// Re-registering an EXISTING session must still work at the ceiling: it
	// takes no new slot, and refusing it would break every reattach once the
	// registry filled.
	if err := r.Register("s-0", "uid:1000", "webchat"); err != nil {
		t.Errorf("re-register at ceiling: %v", err)
	}
	// Freeing a slot makes room again.
	r.Unregister("s-0")
	if err := r.Register("now-there-is-room", "uid:1000", "cli"); err != nil {
		t.Errorf("after freeing a slot: %v", err)
	}

	g := New(Options{})
	for i := 0; i < GrantsMax; i++ {
		if err := g.Grant(fmt.Sprintf("uid:from-%d", i), "uid:to"); err != nil {
			t.Fatalf("grant %d: %v", i, err)
		}
	}
	if err := g.Grant("uid:one-too-many", "uid:to"); !errors.Is(err, ErrGrantsFull) {
		t.Fatalf("grant ceiling: err = %v; want ErrGrantsFull", err)
	}
	// Re-granting an existing pair is idempotent and takes no new slot.
	if err := g.Grant("uid:from-0", "uid:to"); err != nil {
		t.Errorf("re-grant at ceiling: %v", err)
	}
	// Revoking frees one.
	g.Revoke("uid:from-0", "uid:to")
	if err := g.Grant("uid:new", "uid:to"); err != nil {
		t.Errorf("after revoke: %v", err)
	}
}

// Labels are THIS module's state, and the local map is authoritative for them.
//
// An earlier version deferred label resolution to the directory, on the reading
// that a peer label is db1's server_sessions.title. That column has no writer at
// all: the session insert stores the literal empty string and nothing updates
// it. Deferring to a writer that does not exist would have left peer addressing
// with nobody able to set a name.
//
// A title is a display name; a label is the handle peer messaging is addressed
// by. Matching them by column shape rather than meaning was the error.
func TestLabelsAreOwnedHereAndConfirmedAgainstTheDirectory(t *testing.T) {
	r, _ := newTestRegistry(t, "A", "B")
	if err := r.SetLabel("A", "reviewer"); err != nil {
		t.Fatal(err)
	}

	got, ok := r.Lookup("uid:1000", "reviewer")
	if !ok || got != "A" {
		t.Fatalf("lookup = %q,%v; want A,true", got, ok)
	}
	// Scoped by owner: a label is unique per principal, not globally.
	if _, ok := r.Lookup("uid:9999", "reviewer"); ok {
		t.Error("a label resolved for the wrong owner")
	}

	// The one thing that is NOT local is whether the session still exists. A
	// label pointing at a departed session must not be handed out, or a caller
	// is sent somewhere unreachable.
	r.SetSessionOwner(func(id string) (string, bool) { return "uid:1000", id != "A" })
	if got, ok := r.Lookup("uid:1000", "reviewer"); ok {
		t.Errorf("resolved %q to a session the directory says is gone", got)
	}
	// With the directory agreeing it exists, it resolves again.
	r.SetSessionOwner(func(id string) (string, bool) { return "uid:1000", true })
	if _, ok := r.Lookup("uid:1000", "reviewer"); !ok {
		t.Error("a live session's label did not resolve")
	}
}

// Existence must be answered by whoever owns the directory, for the same reason
// resolution is -- and this one is the more dangerous of the pair.
//
// The registry only holds an entry for a session it has SEEN: one that
// registered in process, or that somebody has messaged. A session db1 knows
// about but nobody has yet written to has no entry here. Answering "no such
// session" for it INVERTS the distinction the inbox stage exists to make: the
// truthful answer is "no mail", and a caller told "no such peer" stops asking.
func TestExistsAsksTheDirectoryForSessionsItHasNotSeen(t *testing.T) {
	r, _ := newTestRegistry(t, "seen")

	// Without a hook, only what the registry has seen exists.
	if !r.Exists("seen") {
		t.Error("a registered session should exist")
	}
	if r.Exists("known-to-db1") {
		t.Error("fallback claimed a session it has never seen")
	}

	var asked []string
	r.SetSessionOwner(func(id string) (string, bool) {
		asked = append(asked, id)
		return "uid:1000", id == "known-to-db1"
	})

	// The directory knows it, so it exists and its inbox is merely EMPTY.
	if !r.Exists("known-to-db1") {
		t.Error("a session the directory knows was reported missing")
	}
	if n := r.Len("known-to-db1"); n != 0 {
		t.Errorf("len = %d; want 0 -- no mail, which is not the same as no session", n)
	}
	// The directory does not know this one, so it really is missing.
	if r.Exists("never-existed") {
		t.Error("a session the directory denies was reported present")
	}

	// The directory OUTRANKS a local entry. Holding mail for a session is not
	// evidence the session exists: under the undeliverable rule that mail is
	// exactly what a departed session leaves behind. Exists and Owner therefore
	// share one implementation, because two functions answering nearly the same
	// question is how they came to disagree.
	if r.Exists("seen") {
		t.Error("a locally-held session the directory denies was reported present")
	}

	// Owner comes from the same hook, so a label lookup is scoped against the
	// principal the DIRECTORY reports rather than a local copy of it.
	if owner, ok := r.Owner("known-to-db1"); !ok || owner != "uid:1000" {
		t.Errorf("Owner = %q,%v; want uid:1000,true", owner, ok)
	}
	// The directory denying a session outranks a local entry: holding mail for a
	// session that no longer exists is undeliverable, not addressable.
	if _, ok := r.Owner("seen"); ok {
		t.Error("a locally-held session the directory denies was reported addressable")
	}

	r.SetSessionOwner(nil)
	if r.Exists("known-to-db1") {
		t.Error("clearing the hook did not restore the fallback")
	}
	if owner, ok := r.Owner("seen"); !ok || owner != "uid:1000" {
		t.Errorf("fallback Owner = %q,%v; want uid:1000,true", owner, ok)
	}
}
